
/* pngvalid.c - validate libpng by constructing then reading png files.
 *
 * Last changed in libpng 1.5.0 [September 11, 2010]
 * Copyright (c) 2010 Glenn Randers-Pehrson
 * Written by John C. Bowler
 *
 * This code is released under the libpng license.
 * For conditions of distribution and use, see the disclaimer
 * and license in png.h
 *
 * NOTES:
 *   This is a C program that is intended to be linked against libpng.  It
 *   generates bitmaps internally, stores them as PNG files (using the
 *   sequential write code) then reads them back (using the sequential
 *   read code) and validates that the result has the correct data.
 *
 *   The program can be modified and extended to test the correctness of
 *   transformations performed by libpng.
 */

#include "png.h"
#include "zlib.h"   /* For crc32 */

#include <stdlib.h> /* For malloc */
#include <string.h> /* For memcpy, memset */
#include <math.h>   /* For floor */

/* Unused formal parameter errors are removed using the following macro which is
 * expected to have no bad effects on performance.
 */
#ifndef UNUSED
#  define UNUSED(param) param = param;
#endif

/***************************** EXCEPTION HANDLING *****************************/
#include "contrib/visupng/cexcept.h"
struct png_store;
define_exception_type(struct png_store*);

/* The following are macros to reduce typing everywhere where the well known
 * name 'the_exception_context' must be defined.
 */
#define anon_context(ps) struct exception_context *the_exception_context = \
   &(ps)->exception_context
#define context(ps,fault) anon_context(ps); png_store *fault

/******************************* ERROR UTILITIES ******************************/
static size_t safecat(char *buffer, size_t bufsize, size_t pos,
   PNG_CONST char *cat)
{
   while (pos < bufsize && cat != NULL && *cat != 0)
      buffer[pos++] = *cat++;

   if (pos >= bufsize)
      pos = bufsize-1;

   buffer[pos] = 0;
   return pos;
}

static size_t safecatn(char *buffer, size_t bufsize, size_t pos, int n)
{
   char number[64];
   sprintf(number, "%d", n);
   return safecat(buffer, bufsize, pos, number);
}

static size_t safecatd(char *buffer, size_t bufsize, size_t pos, double d,
    int precision)
{
   char number[64];
   sprintf(number, "%.*f", precision, d);
   return safecat(buffer, bufsize, pos, number);
}

static PNG_CONST char invalid[] = "invalid";
static PNG_CONST char sep[] = ": ";

/* NOTE: this is indexed by ln2(bit_depth)! */
static PNG_CONST char *bit_depths[8] =
{
   "1", "2", "4", "8", "16", invalid, invalid, invalid
};

static PNG_CONST char *colour_types[8] =
{
   "greyscale", invalid, "truecolour", "indexed-colour",
   "greyscale with alpha", invalid, "truecolour with alpha", invalid
};

/* To get log-bit-depth from bit depth, returns 0 to 7 (7 on error). */
static unsigned int
log2depth(png_byte bit_depth)
{
   switch (bit_depth)
   {
      case 1:
         return 0;

      case 2:
         return 1;

      case 4:
         return 2;

      case 8:
         return 3;

      case 16:
         return 4;

      default:
         return 7;
   }
}

/* A numeric ID based on PNG file characteristics: */
#define FILEID(col, depth, interlace) \
   ((png_uint_32)((col) + ((depth)<<3)) + ((interlace)<<8))
#define COL_FROM_ID(id) ((png_byte)((id)& 0x7U))
#define DEPTH_FROM_ID(id) ((png_byte)(((id) >> 3) & 0x1fU))
#define INTERLACE_FROM_ID(id) ((int)(((id) >> 8) & 0xff))

/* Utility to construct a standard name for a standard image. */
static size_t
standard_name(char *buffer, size_t bufsize, size_t pos, png_byte colour_type,
    int log_bit_depth, int interlace_type)
{
   pos = safecat(buffer, bufsize, pos, colour_types[colour_type]);
   pos = safecat(buffer, bufsize, pos, " ");
   pos = safecat(buffer, bufsize, pos, bit_depths[log_bit_depth]);
   pos = safecat(buffer, bufsize, pos, " bit");

   if (interlace_type != PNG_INTERLACE_NONE)
      pos = safecat(buffer, bufsize, pos, " interlaced");

   return pos;
}

static size_t
standard_name_from_id(char *buffer, size_t bufsize, size_t pos, png_uint_32 id)
{
   return standard_name(buffer, bufsize, pos, COL_FROM_ID(id),
      log2depth(DEPTH_FROM_ID(id)), INTERLACE_FROM_ID(id));
}

/* Convenience API and defines to list valid formats.  Note that 16 bit read and
 * write support is required to do 16 bit read tests (we must be able to make a
 * 16 bit image to test!)
 */
#ifdef PNG_WRITE_16BIT_SUPPORTED
#  define WRITE_BDHI 4
#  ifdef PNG_READ_16BIT_SUPPORTED
#     define READ_BDHI 4
#     define DO_16BIT
#  endif
#else
#  define WRITE_BDHI 3
#endif
#ifndef DO_16BIT
#  define READ_BDHI 3
#endif

static int
next_format(png_bytep colour_type, png_bytep bit_depth)
{
   if (*bit_depth == 0)
   {
      *colour_type = 0, *bit_depth = 1;
      return 1;
   }

   *bit_depth = (png_byte)(*bit_depth << 1);

   /* Palette images are restricted to 8 bit depth */
   if (*bit_depth <= 8
#     ifdef DO_16BIT
         || (*colour_type != 3 && *bit_depth <= 16)
#     endif
      )
      return 1;

   /* Move to the next color type, or return 0 at the end. */
   switch (*colour_type)
   {
      case 0:
         *colour_type = 2;
         *bit_depth = 8;
         return 1;

      case 2:
         *colour_type = 3;
         *bit_depth = 1;
         return 1;

      case 3:
         *colour_type = 4;
         *bit_depth = 8;
         return 1;

      case 4:
         *colour_type = 6;
         *bit_depth = 8;
         return 1;

      default:
         return 0;
   }
}

static unsigned int
sample(png_const_bytep row, png_byte colour_type, png_byte bit_depth,
    png_uint_32 x, unsigned int sample)
{
   png_uint_32 index, result;
   
   /* Find a sample index for the desired sample: */
   x *= bit_depth;
   index = x;

   if ((colour_type & 1) == 0) /* !palette */
   {
      if (colour_type & 2)
         index *= 3, index += sample; /* Colour channels; select one */

      if (colour_type & 4)
         index += x; /* Alpha channel */
   }

   /* Return the sample from the row as an integer. */
   row += index >> 3;
   result = *row;

   if (bit_depth == 8)
      return result;

   else if (bit_depth > 8)
      return (result << 8) + *++row;

   /* Less than 8 bits per sample. */
   index &= 7;
   return (result >> (8-index-bit_depth)) & ((1U<<bit_depth)-1);
}

/*************************** BASIC PNG FILE WRITING ***************************/
/* A png_store takes data from the sequential writer or provides data
 * to the sequential reader.  It can also store the result of a PNG
 * write for later retrieval.
 */
#define STORE_BUFFER_SIZE 500 /* arbitrary */
typedef struct png_store_buffer
{
   struct png_store_buffer*  prev;    /* NOTE: stored in reverse order */
   png_byte                  buffer[STORE_BUFFER_SIZE];
} png_store_buffer;

#define FILE_NAME_SIZE 64

typedef struct png_store_file
{
   struct png_store_file*  next;      /* as many as you like... */
   char                    name[FILE_NAME_SIZE];
   png_uint_32             id;        /* must be correct (see FILEID) */
   png_size_t              datacount; /* In this (the last) buffer */
   png_store_buffer        data;      /* Last buffer in file */
} png_store_file;

/* The following is a pool of memory allocated by a single libpng read or write
 * operation.
 */
typedef struct store_pool
{
   struct png_store    *store;   /* Back pointer */
   struct store_memory *list;    /* List of allocated memory */
   png_byte             mark[4]; /* Before and after data */

   /* Statistics for this run. */
   png_alloc_size_t     max;     /* Maximum single allocation */
   png_alloc_size_t     current; /* Current allocation */
   png_alloc_size_t     limit;   /* Highest current allocation */
   png_alloc_size_t     total;   /* Total allocation */

   /* Overall statistics (retained across successive runs). */
   png_alloc_size_t     max_max;
   png_alloc_size_t     max_limit;
   png_alloc_size_t     max_total;
} store_pool;

typedef struct png_store
{
   /* For cexcept.h exception handling - simply store one of these;
    * the context is a self pointer but it may point to a different
    * png_store (in fact it never does in this program.)
    */
   struct exception_context
                      exception_context;

   unsigned int       verbose :1;
   unsigned int       treat_warnings_as_errors :1;
   unsigned int       expect_error :1;
   unsigned int       expect_warning :1;
   unsigned int       saw_warning :1;
   unsigned int       speed :1;
   unsigned int       progressive :1; /* use progressive read */
   unsigned int       validated :1;   /* used as a temporary flag */
   int                nerrors;
   int                nwarnings;
   char               test[64]; /* Name of test */
   char               error[128];

   /* Read fields */
   png_structp        pread;    /* Used to read a saved file */
   png_infop          piread;
   png_store_file*    current;  /* Set when reading */
   png_store_buffer*  next;     /* Set when reading */
   png_size_t         readpos;  /* Position in *next */
   png_byte*          image;    /* Buffer for reading interlaced images */
   size_t             cb_image; /* Size of this buffer */
   store_pool         read_memory_pool;

   /* Write fields */
   png_store_file*    saved;
   png_structp        pwrite;   /* Used when writing a new file */
   png_infop          piwrite;
   png_size_t         writepos; /* Position in .new */
   char               wname[FILE_NAME_SIZE];
   png_store_buffer   new;      /* The end of the new PNG file being written. */
   store_pool         write_memory_pool;
} png_store;

/* Initialization and cleanup */
static void
store_pool_mark(png_byte *mark)
{
   /* Generate a new mark.  This uses a boring repeatable algorihtm and it is
    * implemented here so that it gives the same set of numbers on every
    * architecture.  It's a linear congruential generator (Knuth or Sedgewick
    * "Algorithms") but it comes from the 'feedback taps' table in Horowitz and
    * Hill, "The Art of Electronics".
    */
   static png_uint_32 u0 = 0x12345678, u1 = 1;

   /* There are thirty three bits, the next bit in the sequence is bit-33 XOR
    * bit-20.  The top 1 bit is in u1, the bottom 32 are in u0.
    */
   int i;
   for (i=0; i<4; ++i)
   {
      /* First generate 8 new bits then shift them in at the end. */
      png_uint_32 u = ((u0 >> (20-8)) ^ ((u1 << 7) | (u0 >> (32-7)))) & 0xff;
      u1 <<= 8;
      u1 |= u0 >> 24;
      u0 <<= 8;
      u0 |= u;
      *mark++ = (png_byte)u;
   }
}

static void
store_pool_init(png_store *ps, store_pool *pool)
{
   memset(pool, 0, sizeof *pool);

   pool->store = ps;
   pool->list = NULL;
   pool->max = pool->current = pool->limit = pool->total = 0;
   pool->max_max = pool->max_limit = pool->max_total = 0;
   store_pool_mark(pool->mark);
}

static void
store_init(png_store* ps)
{
   memset(ps, 0, sizeof *ps);
   init_exception_context(&ps->exception_context);
   store_pool_init(ps, &ps->read_memory_pool);
   store_pool_init(ps, &ps->write_memory_pool);
   ps->verbose = 0;
   ps->treat_warnings_as_errors = 0;
   ps->expect_error = 0;
   ps->expect_warning = 0;
   ps->saw_warning = 0;
   ps->speed = 0;
   ps->progressive = 0;
   ps->validated = 0;
   ps->nerrors = ps->nwarnings = 0;
   ps->pread = NULL;
   ps->piread = NULL;
   ps->saved = ps->current = NULL;
   ps->next = NULL;
   ps->readpos = 0;
   ps->image = NULL;
   ps->cb_image = 0;
   ps->pwrite = NULL;
   ps->piwrite = NULL;
   ps->writepos = 0;
   ps->new.prev = NULL;
}

/* This somewhat odd function is used when reading an image to ensure that the
 * buffer is big enough - this is why a png_structp is available.
 */
static void
store_ensure_image(png_store *ps, png_structp pp, size_t cb)
{
   if (ps->cb_image < cb)
   {
      if (ps->image != NULL)
      {
         free(ps->image-1);
         ps->cb_image = 0;
      }

      /* The buffer is deliberately mis-aligned. */
      ps->image = malloc(cb+1);
      if (ps->image == NULL)
         png_error(pp, "OOM allocating image buffer");

      ++(ps->image);
      ps->cb_image = cb;
   }
}

static void
store_freebuffer(png_store_buffer* psb)
{
   if (psb->prev)
   {
      store_freebuffer(psb->prev);
      free(psb->prev);
      psb->prev = NULL;
   }
}

static void
store_freenew(png_store *ps)
{
   store_freebuffer(&ps->new);
   ps->writepos = 0;
}

static void
store_storenew(png_store *ps)
{
   png_store_buffer *pb;

   if (ps->writepos != STORE_BUFFER_SIZE)
      png_error(ps->pwrite, "invalid store call");

   pb = malloc(sizeof *pb);

   if (pb == NULL)
      png_error(ps->pwrite, "store new: OOM");

   *pb = ps->new;
   ps->new.prev = pb;
   ps->writepos = 0;
}

static void
store_freefile(png_store_file **ppf)
{
   if (*ppf != NULL)
   {
      store_freefile(&(*ppf)->next);

      store_freebuffer(&(*ppf)->data);
      (*ppf)->datacount = 0;
      free(*ppf);
      *ppf = NULL;
   }
}

/* Main interface to file storeage, after writing a new PNG file (see the API
 * below) call store_storefile to store the result with the given name and id.
 */
static void
store_storefile(png_store *ps, png_uint_32 id)
{
   png_store_file *pf = malloc(sizeof *pf);
   if (pf == NULL)
      png_error(ps->pwrite, "storefile: OOM");
   safecat(pf->name, sizeof pf->name, 0, ps->wname);
   pf->id = id;
   pf->data = ps->new;
   pf->datacount = ps->writepos;
   ps->new.prev = NULL;
   ps->writepos = 0;

   /* And save it. */
   pf->next = ps->saved;
   ps->saved = pf;
}

/* Generate an error message (in the given buffer) */
static size_t
store_message(png_store *ps, png_structp pp, char *buffer, size_t bufsize,
   size_t pos, PNG_CONST char *msg)
{
   if (pp != NULL && pp == ps->pread)
   {
      /* Reading a file */
      pos = safecat(buffer, bufsize, pos, "read: ");

      if (ps->current != NULL)
      {
         pos = safecat(buffer, bufsize, pos, ps->current->name);
         pos = safecat(buffer, bufsize, pos, sep);
      }
   }

   else if (pp != NULL && pp == ps->pwrite)
   {
      /* Writing a file */
      pos = safecat(buffer, bufsize, pos, "write: ");
      pos = safecat(buffer, bufsize, pos, ps->wname);
      pos = safecat(buffer, bufsize, pos, sep);
   }

   else
   {
      /* Neither reading nor writing (or a memory error in struct delete) */
      pos = safecat(buffer, bufsize, pos, "pngvalid: ");
   }

   if (ps->test[0] != 0)
   {
      pos = safecat(buffer, bufsize, pos, ps->test);
      pos = safecat(buffer, bufsize, pos, sep);
   }
   pos = safecat(buffer, bufsize, pos, msg);
   return pos;
}

/* Log an error or warning - the relevant count is always incremented. */
static void
store_log(png_store* ps, png_structp pp, png_const_charp message, int is_error)
{
   /* The warning is copied to the error buffer if there are no errors and it is
    * the first warning.  The error is copied to the error buffer if it is the
    * first error (overwriting any prior warnings).
    */
   if (is_error ? (ps->nerrors)++ == 0 :
       (ps->nwarnings)++ == 0 && ps->nerrors == 0)
      store_message(ps, pp, ps->error, sizeof ps->error, 0, message);

   if (ps->verbose)
   {
      char buffer[256];
      size_t pos;

      if (is_error)
         pos = safecat(buffer, sizeof buffer, 0, "error: ");
      else
         pos = safecat(buffer, sizeof buffer, 0, "warning: ");
         
      store_message(ps, pp, buffer, sizeof buffer, pos, message);
      fputs(buffer, stderr);
      fputc('\n', stderr);
   }
}

/* Functions to use as PNG callbacks. */
static void
store_error(png_structp pp, png_const_charp message) /* PNG_NORETURN */
{
   png_store *ps = png_get_error_ptr(pp);

   if (!ps->expect_error)
      store_log(ps, pp, message, 1/*error*/);

   /* And finally throw an exception. */
   {
      struct exception_context *the_exception_context = &ps->exception_context;
      Throw ps;
   }
}

static void
store_warning(png_structp pp, png_const_charp message)
{
   png_store *ps = png_get_error_ptr(pp);

   if (!ps->expect_warning)
      store_log(ps, pp, message, 0/*warning*/);
   else
      ps->saw_warning = 1;
}

static void
store_write(png_structp pp, png_bytep pb, png_size_t st)
{
   png_store *ps = png_get_io_ptr(pp);

   if (ps->pwrite != pp)
      png_error(pp, "store state damaged");

   while (st > 0)
   {
      size_t cb;

      if (ps->writepos >= STORE_BUFFER_SIZE)
         store_storenew(ps);

      cb = st;

      if (cb > STORE_BUFFER_SIZE - ps->writepos)
         cb = STORE_BUFFER_SIZE - ps->writepos;

      memcpy(ps->new.buffer + ps->writepos, pb, cb);
      pb += cb;
      st -= cb;
      ps->writepos += cb;
   }
}

static void
store_flush(png_structp pp)
{
   pp = pp;
   /*DOES NOTHING*/
}

static size_t
store_read_buffer_size(png_store *ps)
{
   /* Return the bytes available for read in the current buffer. */
   if (ps->next != &ps->current->data)
      return STORE_BUFFER_SIZE;

   return ps->current->datacount;
}

/* Return total bytes available for read. */
static size_t
store_read_buffer_avail(png_store *ps)
{
   if (ps->current != NULL && ps->next != NULL)
   {
      png_store_buffer *next = &ps->current->data;
      size_t cbAvail = ps->current->datacount;

      while (next != ps->next && next != NULL)
      {
         next = next->prev;
         cbAvail += STORE_BUFFER_SIZE;
      }

      if (next != ps->next)
         png_error(ps->pread, "buffer read error");

      if (cbAvail > ps->readpos)
         return cbAvail - ps->readpos;
   }

   return 0;
}

static int
store_read_buffer_next(png_store *ps)
{
   png_store_buffer *pbOld = ps->next;
   png_store_buffer *pbNew = &ps->current->data;
   if (pbOld != pbNew)
   {
      while (pbNew != NULL && pbNew->prev != pbOld)
         pbNew = pbNew->prev;

      if (pbNew != NULL)
      {
         ps->next = pbNew;
         ps->readpos = 0;
         return 1;
      }

      png_error(ps->pread, "buffer lost");
   }

   return 0; /* EOF or error */
}

/* Need separate implementation and callback to allow use of the same code
 * during progressive read, where the io_ptr is set internally by libpng.
 */
static void
store_read_imp(png_store *ps, png_bytep pb, png_size_t st)
{
   if (ps->current == NULL || ps->next == NULL)
      png_error(ps->pread, "store state damaged");

   while (st > 0)
   {
      size_t cbAvail = store_read_buffer_size(ps) - ps->readpos;

      if (cbAvail > 0)
      {
         if (cbAvail > st) cbAvail = st;
         memcpy(pb, ps->next->buffer + ps->readpos, cbAvail);
         st -= cbAvail;
         pb += cbAvail;
         ps->readpos += cbAvail;
      }

      else if (!store_read_buffer_next(ps))
         png_error(ps->pread, "read beyond end of file");
   }
}

static void
store_read(png_structp pp, png_bytep pb, png_size_t st)
{
   png_store *ps = png_get_io_ptr(pp);

   if (ps == NULL || ps->pread != pp)
      png_error(pp, "bad store read call");

   store_read_imp(ps, pb, st);
}

static void
store_progressive_read(png_store *ps, png_structp pp, png_infop pi)
{
   /* Notice that a call to store_read will cause this function to fail because
    * readpos will be set.
    */
   if (ps->pread != pp || ps->current == NULL || ps->next == NULL)
      png_error(pp, "store state damaged (progressive)");

   do
   {
      if (ps->readpos != 0)
         png_error(pp, "store_read called during progressive read");

      png_process_data(pp, pi, ps->next->buffer, store_read_buffer_size(ps));
   }
   while (store_read_buffer_next(ps));
}

/***************************** MEMORY MANAGEMENT*** ***************************/
/* A store_memory is simply the header for an allocated block of memory.  The
 * pointer returned to libpng is just after the end of the header block, the
 * allocated memory is followed by a second copy of the 'mark'.
 */
typedef struct store_memory
{
   store_pool          *pool;    /* Originating pool */
   struct store_memory *next;    /* Singly linked list */
   png_alloc_size_t     size;    /* Size of memory allocated */
   png_byte             mark[4]; /* ID marker */
} store_memory;

/* Handle a fatal error in memory allocation.  This calls png_error if the
 * libpng struct is non-NULL, else it outputs a message and returns.  This means
 * that a memory problem while libpng is running will abort (png_error) the
 * handling of particular file while one in cleanup (after the destroy of the
 * struct has returned) will simply keep going and free (or attempt to free)
 * all the memory.
 */
static void
store_pool_error(png_store *ps, png_structp pp, PNG_CONST char *msg)
{
   if (pp != NULL)
      png_error(pp, msg);

   /* Else we have to do it ourselves.  png_error eventually calls store_log,
    * above.  store_log accepts a NULL png_structp - it just changes what gets
    * output by store_message.
    */
   store_log(ps, pp, msg, 1/*error*/);
}

static void
store_memory_free(png_structp pp, store_pool *pool, store_memory *memory)
{
   /* Note that pp may be NULL (see store_pool_delete below), the caller has
    * found 'memory' in pool->list *and* unlinked this entry, so this is a valid
    * pointer (for sure), but the contents may have been trashed.
    */
   if (memory->pool != pool)
      store_pool_error(pool->store, pp, "memory corrupted (pool)");

   else if (memcmp(memory->mark, pool->mark, sizeof memory->mark) != 0)
      store_pool_error(pool->store, pp, "memory corrupted (start)");

   /* It should be safe to read the size field now. */
   else
   {
      png_alloc_size_t cb = memory->size;

      if (cb > pool->max)
         store_pool_error(pool->store, pp, "memory corrupted (size)");

      else if (memcmp((png_bytep)(memory+1)+cb, pool->mark, sizeof pool->mark)
         != 0)
         store_pool_error(pool->store, pp, "memory corrupted (end)");

      /* Finally give the library a chance to find problems too: */
      else
         {
         pool->current -= cb;
         free(memory);
         }
   }
}

static void
store_pool_delete(png_store *ps, store_pool *pool)
{
   if (pool->list != NULL)
   {
      fprintf(stderr, "%s: %s %s: memory lost (list follows):\n", ps->test,
         pool == &ps->read_memory_pool ? "read" : "write",
         pool == &ps->read_memory_pool ? (ps->current != NULL ?
            ps->current->name : "unknown file") : ps->wname);
      ++ps->nerrors;

      do
      {
         store_memory *next = pool->list;
         pool->list = next->next;
         next->next = NULL;

         fprintf(stderr, "\t%ud bytes @ %p\n", next->size, next+1);
         /* The NULL means this will always return, even if the memory is
          * corrupted.
          */
         store_memory_free(NULL, pool, next);
      }
      while (pool->list != NULL);
   }

   /* And reset the other fields too for the next time. */
   if (pool->max > pool->max_max) pool->max_max = pool->max;
   pool->max = 0;
   if (pool->current != 0) /* unexpected internal error */
      fprintf(stderr, "%s: %s %s: memory counter mismatch (internal error)\n",
         ps->test, pool == &ps->read_memory_pool ? "read" : "write",
         pool == &ps->read_memory_pool ? (ps->current != NULL ?
            ps->current->name : "unknown file") : ps->wname);
   pool->current = 0;
   if (pool->limit > pool->max_limit) pool->max_limit = pool->limit;
   pool->limit = 0;
   if (pool->total > pool->max_total) pool->max_total = pool->total;
   pool->total = 0;

   /* Get a new mark too. */
   store_pool_mark(pool->mark);
}

/* The memory callbacks: */
static png_voidp
store_malloc(png_structp pp, png_alloc_size_t cb)
{
   store_pool *pool = png_get_mem_ptr(pp);
   store_memory *new = malloc(cb + (sizeof *new) + (sizeof pool->mark));

   if (new != NULL)
   {
      if (cb > pool->max) pool->max = cb;
      pool->current += cb;
      if (pool->current > pool->limit) pool->limit = pool->current;
      pool->total += cb;

      new->size = cb;
      memcpy(new->mark, pool->mark, sizeof new->mark);
      memcpy((png_byte*)(new+1) + cb, pool->mark, sizeof pool->mark);
      new->pool = pool;
      new->next = pool->list;
      pool->list = new;
      ++new;
   }
   else
      store_pool_error(pool->store, pp, "out of memory");

   return new;
}

static void
store_free(png_structp pp, png_voidp memory)
{
   store_pool *pool = png_get_mem_ptr(pp);
   store_memory *this = memory, **test;

   /* First check that this 'memory' really is valid memory - it must be in the
    * pool list.  If it is use the shared memory_free function to free it.
    */
   --this;
   for (test = &pool->list; *test != this; test = &(*test)->next)
   {
      if (*test == NULL)
      {
         store_pool_error(pool->store, pp, "bad pointer to free");
         return;
      }
   }

   /* Unlink this entry, *test == this. */
   *test = this->next;
   this->next = NULL;
   store_memory_free(pp, pool, this);
}

/* Setup functions. */
/* Cleanup when aborting a write or after storing the new file. */
static void
store_write_reset(png_store *ps)
{
   if (ps->pwrite != NULL)
   {
      anon_context(ps);

      Try
         png_destroy_write_struct(&ps->pwrite, &ps->piwrite);

      Catch_anonymous
      {
         /* memory corruption: continue. */
      }

      ps->pwrite = NULL;
      ps->piwrite = NULL;
   }

   /* And make sure that all the memory has been freed - this will output
    * spurious errors in the case of memory corruption above, but this is safe.
    */
   store_pool_delete(ps, &ps->write_memory_pool);
   
   store_freenew(ps);
}

/* The following is the main write function, it returns a png_struct and,
 * optionally, a png)info suitable for writiing a new PNG file.  Use
 * store_storefile above to record this file after it has been written.  The
 * returned libpng structures as destroyed by store_write_reset above.
 */
static png_structp
set_store_for_write(png_store *ps, png_infopp ppi,
   PNG_CONST char * volatile name)
{
   anon_context(ps);

   Try
   {
      if (ps->pwrite != NULL)
         png_error(ps->pwrite, "write store already in use");

      store_write_reset(ps);
      safecat(ps->wname, sizeof ps->wname, 0, name);

      /* Don't do the slow memory checks if doing a speed test. */
      if (ps->speed)
         ps->pwrite = png_create_write_struct(PNG_LIBPNG_VER_STRING,
            ps, store_error, store_warning);
      else
         ps->pwrite = png_create_write_struct_2(PNG_LIBPNG_VER_STRING,
            ps, store_error, store_warning, &ps->write_memory_pool,
            store_malloc, store_free);
      png_set_write_fn(ps->pwrite, ps, store_write, store_flush);

      if (ppi != NULL)
         *ppi = ps->piwrite = png_create_info_struct(ps->pwrite);
   }

   Catch_anonymous
      return NULL;

   return ps->pwrite;
}

/* Cleanup when finished reading (either due to error or in the success case. )
 */
static void
store_read_reset(png_store *ps)
{
   if (ps->pread != NULL)
   {
      anon_context(ps);
      
      Try
         png_destroy_read_struct(&ps->pread, &ps->piread, NULL);

      Catch_anonymous
      {
         /*error already output: continue*/
      }

      ps->pread = NULL;
      ps->piread = NULL;
   }

   /* Always do this to be safe. */
   store_pool_delete(ps, &ps->read_memory_pool);

   ps->current = NULL;
   ps->next = NULL;
   ps->readpos = 0;
   ps->validated = 0;
}

static void
store_read_set(png_store *ps, png_uint_32 id)
{
   png_store_file *pf = ps->saved;

   while (pf != NULL)
   {
      if (pf->id == id)
      {
         ps->current = pf;
         ps->next = NULL;
         store_read_buffer_next(ps);
         return;
      }

      pf = pf->next;
   }

      {
      size_t pos;
      char msg[FILE_NAME_SIZE+64];

      pos = standard_name_from_id(msg, sizeof msg, 0, id);
      pos = safecat(msg, sizeof msg, pos, ": file not found");
      png_error(ps->pread, msg);
      }
}

/* The main interface for reading a saved file - pass the id number of the file
 * to retrieve.  Ids must be unique or the earlier file will be hidden.  The API
 * returns a png_struct and, optionally, a png_info.  Both of these will be
 * destroyed by store_read_reset above.
 */
static png_structp
set_store_for_read(png_store *ps, png_infopp ppi, png_uint_32 id,
   PNG_CONST char *name)
{
   /* Set the name for png_error */
   safecat(ps->test, sizeof ps->test, 0, name);

   if (ps->pread != NULL)
      png_error(ps->pread, "read store already in use");

   store_read_reset(ps);

   /* Both the create APIs can return NULL if used in their default mode
    * (because there is no other way of handling an error because the jmp_buf by
    * default is stored in png_struct and that has not been allocated!)
    * However, given that store_error works correctly in these circumstances we
    * don't ever expect NULL in this program.
    */
   if (ps->speed)
      ps->pread = png_create_read_struct(PNG_LIBPNG_VER_STRING, ps,
          store_error, store_warning);
   else
      ps->pread = png_create_read_struct_2(PNG_LIBPNG_VER_STRING, ps,
          store_error, store_warning, &ps->read_memory_pool, store_malloc,
          store_free);

   if (ps->pread == NULL)
   {
      struct exception_context *the_exception_context = &ps->exception_context;

      store_log(ps, NULL, "png_create_read_struct returned NULL (unexpected)",
         1/*error*/);

      Throw ps;
   }
      
   store_read_set(ps, id);

   if (ppi != NULL)
      *ppi = ps->piread = png_create_info_struct(ps->pread);

   return ps->pread;
}

/* The overall cleanup of a store simply calls the above then removes all the
 * saved files.  This does not delete the store itself.
 */
static void
store_delete(png_store *ps)
{
   store_write_reset(ps);
   store_read_reset(ps);
   store_freefile(&ps->saved);
   if (ps->image != NULL)
   {
      free(ps->image-1);
      ps->image = NULL;
      ps->cb_image = 0;
   }
}

/*********************** PNG FILE MODIFICATION ON READ ************************/
/* Files may be modified on read.  The following structure contains a complete
 * png_store together with extra members to handle modification and a special
 * read callback for libpng.  To use this the 'modifications' field must be set
 * to a list of png_modification structures that actually perform the
 * modification, otherwise a png_modifier is functionally equivalent to a
 * png_store.  There is a special read function, set_modifier_for_read, which
 * replaces set_store_for_read.
 */
typedef struct png_modifier
{
   png_store               this;             /* I am a png_store */
   struct png_modification *modifications;   /* Changes to make */

   enum modifier_state
   {
      modifier_start,                        /* Initial value */
      modifier_signature,                    /* Have a signature */
      modifier_IHDR                          /* Have an IHDR */
   }                        state;           /* My state */

   /* Information from IHDR: */
   png_byte                 bit_depth;       /* From IHDR */
   png_byte                 colour_type;     /* From IHDR */

   /* While handling PLTE, IDAT and IEND these chunks may be pended to allow
    * other chunks to be inserted.
    */
   png_uint_32              pending_len;
   png_uint_32              pending_chunk;

   /* Test values */
   double                  *gammas;
   unsigned int             ngammas;

   /* Lowest sbit to test (libpng fails for sbit < 8) */
   png_byte                 sbitlow;

   /* Error control - these are the limits on errors accepted by the gamma tests
    * below.
    */
   double                   maxout8;  /* Maximum output value error */
   double                   maxabs8;  /* Absolute sample error 0..1 */
   double                   maxpc8;   /* Percentage sample error 0..100% */
   double                   maxout16; /* Maximum output value error */
   double                   maxabs16; /* Absolute sample error 0..1 */
   double                   maxpc16;  /* Percentage sample error 0..100% */

   /* Logged 8 and 16 bit errors ('output' values): */
   double                   error_gray_2;
   double                   error_gray_4;
   double                   error_gray_8;
   double                   error_gray_16;
   double                   error_color_8;
   double                   error_color_16;

   /* Flags: */
   /* Whether or not to interlace. */
   int                      interlace_type :9; /* int, but must store '1' */

   /* When to use the use_input_precision option: */
   unsigned int             use_input_precision :1;
   unsigned int             use_input_precision_sbit :1;
   unsigned int             use_input_precision_16to8 :1;
   unsigned int             log :1;   /* Log max error */

   /* Buffer information, the buffer size limits the size of the chunks that can
    * be modified - they must fit (including header and CRC) into the buffer!
    */
   size_t                   flush;           /* Count of bytes to flush */
   size_t                   buffer_count;    /* Bytes in buffer */
   size_t                   buffer_position; /* Position in buffer */
   png_byte                 buffer[1024];
} png_modifier;

static double abserr(png_modifier *pm, png_byte bit_depth)
{
   return bit_depth == 16 ? pm->maxabs16 : pm->maxabs8;
}

static double pcerr(png_modifier *pm, png_byte bit_depth)
{
   return (bit_depth == 16 ? pm->maxpc16 : pm->maxpc8) * .01;
}

static double outerr(png_modifier *pm, png_byte bit_depth)
{
   /* There is a serious error in the 2 and 4 bit grayscale transform because
    * the gamma table value (8 bits) is simply shifted, not rouned, so the
    * error in 4 bit greyscale gamma is up to the value below.  This is a hack
    * to allow pngvalid to succeed:
    */
   if (bit_depth == 2)
      return .73182-.5;

   if (bit_depth == 4)
      return .90644-.5;

   if (bit_depth == 16)
     return pm->maxout16;

   return pm->maxout8;
}

/* This returns true if the test should be stopped now because it has already
 * failed and it is running silently.
 */
static int fail(png_modifier *pm)
{
   return !pm->log && !pm->this.verbose && (pm->this.nerrors > 0 ||
       (pm->this.treat_warnings_as_errors && pm->this.nwarnings > 0));
}

static void
modifier_init(png_modifier *pm)
{
   memset(pm, 0, sizeof *pm);
   store_init(&pm->this);
   pm->modifications = NULL;
   pm->state = modifier_start;
   pm->sbitlow = 1U;
   pm->maxout8 = pm->maxpc8 = pm->maxabs8 = 0;
   pm->maxout16 = pm->maxpc16 = pm->maxabs16 = 0;
   pm->error_gray_2 = pm->error_gray_4 = pm->error_gray_8 = 0;
   pm->error_gray_16 = pm->error_color_8 = pm->error_color_16 = 0;
   pm->interlace_type = PNG_INTERLACE_NONE;
   pm->use_input_precision = 0;
   pm->use_input_precision_sbit = 0;
   pm->use_input_precision_16to8 = 0;
   pm->log = 0;

   /* Rely on the memset for all the other fields - there are no pointers */
}

/* One modification structure must be provided for each chunk to be modified (in
 * fact more than one can be provided if multiple separate changes are desired
 * for a single chunk.)  Modifications include adding a new chunk when a
 * suitable chunk does not exist.
 *
 * The caller of modify_fn will reset the CRC of the chunk and record 'modified'
 * or 'added' as appropriate if the modify_fn returns 1 (true).  If the
 * modify_fn is NULL the chunk is simply removed.
 */
typedef struct png_modification
{
   struct png_modification *next;
   png_uint_32              chunk;

   /* If the following is NULL all matching chunks will be removed: */
   int                    (*modify_fn)(struct png_modifier *pm,
                               struct png_modification *me, int add);

   /* If the following is set to PLTE, IDAT or IEND and the chunk has not been
    * found and modified (and there is a modify_fn) the modify_fn will be called
    * to add the chunk before the relevant chunk.
    */
   png_uint_32              add;
   unsigned int             modified :1;     /* Chunk was modified */
   unsigned int             added    :1;     /* Chunk was added */
   unsigned int             removed  :1;     /* Chunk was removed */
} png_modification;

static void modification_reset(png_modification *pmm)
{
   if (pmm != NULL)
   {
      pmm->modified = 0;
      pmm->added = 0;
      pmm->removed = 0;
      modification_reset(pmm->next);
   }
}

static void
modification_init(png_modification *pmm)
{
   memset(pmm, 0, sizeof *pmm);
   pmm->next = NULL;
   pmm->chunk = 0;
   pmm->modify_fn = NULL;
   pmm->add = 0;
   modification_reset(pmm);
}

static void
modifier_reset(png_modifier *pm)
{
   store_read_reset(&pm->this);
   pm->modifications = NULL;
   pm->state = modifier_start;
   pm->bit_depth = pm->colour_type = 0;
   pm->pending_len = pm->pending_chunk = 0;
   pm->flush = pm->buffer_count = pm->buffer_position = 0;
}

/* Convenience macros. */
#define CHUNK(a,b,c,d) (((a)<<24)+((b)<<16)+((c)<<8)+(d))
#define CHUNK_IHDR CHUNK(73,72,68,82)
#define CHUNK_PLTE CHUNK(80,76,84,69)
#define CHUNK_IDAT CHUNK(73,68,65,84)
#define CHUNK_IEND CHUNK(73,69,78,68)
#define CHUNK_cHRM CHUNK(99,72,82,77)
#define CHUNK_gAMA CHUNK(103,65,77,65)
#define CHUNK_sBIT CHUNK(115,66,73,84)
#define CHUNK_sRGB CHUNK(115,82,71,66)

/* The guts of modification are performed during a read. */
static void
modifier_crc(png_bytep buffer)
{
   /* Recalculate the chunk CRC - a complete chunk must be in
    * the buffer, at the start.
    */
   uInt datalen = png_get_uint_32(buffer);
   png_save_uint_32(buffer+datalen+8, crc32(0L, buffer+4, datalen+4));
}

static void
modifier_setbuffer(png_modifier *pm)
{
   modifier_crc(pm->buffer);
   pm->buffer_count = png_get_uint_32(pm->buffer)+12;
   pm->buffer_position = 0;
}

/* Separate the callback into the actual implementation (which is passed the
 * png_modifier explicitly) and the callback, which gets the modifier from the
 * png_struct.
 */
static void
modifier_read_imp(png_modifier *pm, png_bytep pb, png_size_t st)
{
   while (st > 0)
   {
      size_t cb;
      png_uint_32 len, chunk;
      png_modification *mod;

      if (pm->buffer_position >= pm->buffer_count) switch (pm->state)
      {
         static png_byte sign[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
         case modifier_start:
            store_read_imp(&pm->this, pm->buffer, 8); /* size of signature. */
            pm->buffer_count = 8;
            pm->buffer_position = 0;
   
            if (memcmp(pm->buffer, sign, 8) != 0)
               png_error(pm->this.pread, "invalid PNG file signature");
            pm->state = modifier_signature;
            break;
   
         case modifier_signature:
            store_read_imp(&pm->this, pm->buffer, 13+12); /* size of IHDR */
            pm->buffer_count = 13+12;
            pm->buffer_position = 0;
   
            if (png_get_uint_32(pm->buffer) != 13 ||
                png_get_uint_32(pm->buffer+4) != CHUNK_IHDR)
               png_error(pm->this.pread, "invalid IHDR");
   
            /* Check the list of modifiers for modifications to the IHDR. */
            mod = pm->modifications;
            while (mod != NULL)
            {
               if (mod->chunk == CHUNK_IHDR && mod->modify_fn &&
                   (*mod->modify_fn)(pm, mod, 0))
                  {
                  mod->modified = 1;
                  modifier_setbuffer(pm);
                  }
   
               /* Ignore removal or add if IHDR! */
               mod = mod->next;
            }
   
            /* Cache information from the IHDR (the modified one.) */
            pm->bit_depth = pm->buffer[8+8];
            pm->colour_type = pm->buffer[8+8+1];
   
            pm->state = modifier_IHDR;
            pm->flush = 0;
            break;
   
         case modifier_IHDR:
         default:
            /* Read a new chunk and process it until we see PLTE, IDAT or
             * IEND.  'flush' indicates that there is still some data to
             * output from the preceding chunk.
             */
            if ((cb = pm->flush) > 0)
            {
               if (cb > st) cb = st;
               pm->flush -= cb;
               store_read_imp(&pm->this, pb, cb);
               pb += cb;
               st -= cb;
               if (st <= 0) return;
            }
   
            /* No more bytes to flush, read a header, or handle a pending
             * chunk.
             */
            if (pm->pending_chunk != 0)
            {
               png_save_uint_32(pm->buffer, pm->pending_len);
               png_save_uint_32(pm->buffer+4, pm->pending_chunk);
               pm->pending_len = 0;
               pm->pending_chunk = 0;
            }
            else
               store_read_imp(&pm->this, pm->buffer, 8);
   
            pm->buffer_count = 8;
            pm->buffer_position = 0;
   
            /* Check for something to modify or a terminator chunk. */
            len = png_get_uint_32(pm->buffer);
            chunk = png_get_uint_32(pm->buffer+4);
   
            /* Terminators first, they may have to be delayed for added
             * chunks
             */
            if (chunk == CHUNK_PLTE || chunk == CHUNK_IDAT ||
                chunk == CHUNK_IEND)
            {
               mod = pm->modifications;
   
               while (mod != NULL)
               {
                  if ((mod->add == chunk ||
                      (mod->add == CHUNK_PLTE && chunk == CHUNK_IDAT)) &&
                      mod->modify_fn != NULL && !mod->modified && !mod->added)
                  {
                     /* Regardless of what the modify function does do not run
                      * this again.
                      */
                     mod->added = 1;
   
                     if ((*mod->modify_fn)(pm, mod, 1/*add*/))
                     {
                        /* Reset the CRC on a new chunk */
                        if (pm->buffer_count > 0)
                           modifier_setbuffer(pm);

                        else
                           {
                           pm->buffer_position = 0;
                           mod->removed = 1;
                           }
   
                        /* The buffer has been filled with something (we assume)
                         * so output this.  Pend the current chunk.
                         */
                        pm->pending_len = len;
                        pm->pending_chunk = chunk;
                        break; /* out of while */
                     }
                  }
   
                  mod = mod->next;
               }
   
               /* Don't do any further processing if the buffer was modified -
                * otherwise the code will end up modifying a chunk that was just
                * added.
                */
               if (mod != NULL)
                  break; /* out of switch */
            }
   
            /* If we get to here then this chunk may need to be modified.  To do
             * this is must be less than 1024 bytes in total size, otherwise
             * it just gets flushed.
             */
            if (len+12 <= sizeof pm->buffer)
            {
               store_read_imp(&pm->this, pm->buffer+pm->buffer_count,
                   len+12-pm->buffer_count);
               pm->buffer_count = len+12;
   
               /* Check for a modification, else leave it be. */
               mod = pm->modifications;
               while (mod != NULL)
               {
                  if (mod->chunk == chunk)
                  {
                     if (mod->modify_fn == NULL)
                     {
                        /* Remove this chunk */
                        pm->buffer_count = pm->buffer_position = 0;
                        mod->removed = 1;
                        break; /* Terminate the while loop */
                     }

                     else if ((*mod->modify_fn)(pm, mod, 0))
                     {
                        mod->modified = 1;
                        /* The chunk may have been removed: */
                        if (pm->buffer_count == 0)
                        {
                           pm->buffer_position = 0;
                           break;
                        }
                        modifier_setbuffer(pm);
                     }
                  }
   
                  mod = mod->next;
               }
            }

            else
               pm->flush = len+12 - pm->buffer_count; /* data + crc */
   
            /* Take the data from the buffer (if there is any). */
            break;
      }

      /* Here to read from the modifier buffer (not directly from
       * the store, as in the flush case above.)
       */
      cb = pm->buffer_count - pm->buffer_position;

      if (cb > st)
         cb = st;

      memcpy(pb, pm->buffer + pm->buffer_position, cb);
      st -= cb;
      pb += cb;
      pm->buffer_position += cb;
   }
}

/* The callback: */
static void
modifier_read(png_structp pp, png_bytep pb, png_size_t st)
{
   png_modifier *pm = png_get_io_ptr(pp);

   if (pm == NULL || pm->this.pread != pp)
      png_error(pp, "bad modifier_read call");

   modifier_read_imp(pm, pb, st);
}

/* Like store_progressive_read but the data is getting changed as we go so we
 * need a local buffer.
 */
static void
modifier_progressive_read(png_modifier *pm, png_structp pp, png_infop pi)
{
   if (pm->this.pread != pp || pm->this.current == NULL ||
       pm->this.next == NULL)
      png_error(pp, "store state damaged (progressive)");

   /* This is another Horowitz and Hill random noise generator.  In this case
    * the aim is to stress the progressive reader with truely horrible variable
    * buffer sizes in the range 1..500, so a sequence of 9 bit random numbers is
    * generated.  We could probably just count from 1 to 32767 and get as good
    * a result.
    */
   for (;;)
   {
      static png_uint_32 noise = 1;
      png_size_t cb, cbAvail;
      png_byte buffer[512];

      /* Generate 15 more bits of stuff: */
      noise = (noise << 9) | ((noise ^ (noise >> (9-5))) & 0x1ff);
      cb = noise & 0x1ff;

      /* Check that this number of bytes are available (in the current buffer.)
       * (This doesn't quite work - the modifier might delete a chunk; unlikely
       * but possible, it doesn't happen at present because the modifier only
       * adds chunks to standard images.)
       */
      cbAvail = store_read_buffer_avail(&pm->this);
      if (pm->buffer_count > pm->buffer_position)
         cbAvail += pm->buffer_count - pm->buffer_position;

      if (cb > cbAvail)
      {
         /* Check for EOF: */
         if (cbAvail == 0)
            break;

         cb = cbAvail;
      }

      modifier_read_imp(pm, buffer, cb);
      png_process_data(pp, pi, buffer, cb);
   }

   /* Check the invariants at the end (if this fails it's a problem in this
    * file!)
    */
   if (pm->buffer_count > pm->buffer_position ||
       pm->this.next != &pm->this.current->data ||
       pm->this.readpos < pm->this.current->datacount)
      png_error(pp, "progressive read implementation error");
}

/* Set up a modifier. */
static png_structp
set_modifier_for_read(png_modifier *pm, png_infopp ppi, png_uint_32 id,
    PNG_CONST char *name)
{
   /* Do this first so that the modifier fields are cleared even if an error
    * happens allocating the png_struct.  No allocation is done here so no
    * cleanup is required.
    */
   pm->state = modifier_start;
   pm->bit_depth = 0;
   pm->colour_type = 255;

   pm->pending_len = 0;
   pm->pending_chunk = 0;
   pm->flush = 0;
   pm->buffer_count = 0;
   pm->buffer_position = 0;

   return set_store_for_read(&pm->this, ppi, id, name);
}

/***************************** STANDARD PNG FILES *****************************/
/* Standard files - write and save standard files. */
/* The standard files are constructed with rows which fit into a 1024 byte row
 * buffer.  This makes allocation easier below.  Further regardless of the file
 * format every file has 128 pixels (giving 1024 bytes for 64bpp formats).
 *
 * Files are stored with no gAMA or sBIT chunks, with a PLTE only when needed
 * and with an ID derived from the colour type, bit depth and interlace type
 * as above (FILEID).
 */

/* The number of passes is related to the interlace type, there's no libpng API
 * to determine this so we need an inquiry function:
 */
static int
npasses_from_interlace_type(png_structp pp, int interlace_type)
{
   switch (interlace_type)
   {
   default:
      png_error(pp, "invalid interlace type");

   case PNG_INTERLACE_NONE:
      return 1;

   case PNG_INTERLACE_ADAM7:
      return 7;
   }
}

#define STD_WIDTH  128U
#define STD_ROWMAX (STD_WIDTH*8U)

static unsigned int
bit_size(png_structp pp, png_byte colour_type, png_byte bit_depth)
{
   switch (colour_type)
   {
      case 0:  return bit_depth;

      case 2:  return 3*bit_depth;

      case 3:  return bit_depth;

      case 4:  return 2*bit_depth;

      case 6:  return 4*bit_depth;

      default: png_error(pp, "invalid color type");
   }
}

static size_t
standard_rowsize(png_structp pp, png_byte colour_type, png_byte bit_depth)
{
   return (STD_WIDTH * bit_size(pp, colour_type, bit_depth)) / 8;
}

/* standard_wdith(pp, colour_type, bit_depth) current returns the same number
 * every time, so just use a macro:
 */
#define standard_width(pp, colour_type, bit_depth) STD_WIDTH

static png_uint_32
standard_height(png_structp pp, png_byte colour_type, png_byte bit_depth)
{
   switch (bit_size(pp, colour_type, bit_depth))
   {
      case 1:
      case 2:
      case 4:
         return 1;   /* Total of 128 pixels */

      case 8:
         return 2;   /* Total of 256 pixels/bytes */

      case 16:
         return 512; /* Total of 65536 pixels */

      case 24:
      case 32:
         return 512; /* 65536 pixels */

      case 48:
      case 64:
         return 2048;/* 4 x 65536 pixels. */

      default:
         return 0;   /* Error, will be caught later */
   }
}

/* So the maximum standard image size is: */
#define STD_IMAGEMAX (STD_ROWMAX * 2048)

static void
standard_row(png_structp pp, png_byte buffer[STD_ROWMAX], png_byte colour_type,
    png_byte bit_depth, png_uint_32 y)
{
   png_uint_32 v = y << 7;
   png_uint_32 i = 0;

   switch (bit_size(pp, colour_type, bit_depth))
   {
      case 1:
         while (i<128/8) buffer[i] = v & 0xff, v += 17, ++i;
         return;

      case 2:
         while (i<128/4) buffer[i] = v & 0xff, v += 33, ++i;
         return;

      case 4:
         while (i<128/2) buffer[i] = v & 0xff, v += 65, ++i;
         return;

      case 8:
         /* 256 bytes total, 128 bytes in each row set as follows: */
         while (i<128) buffer[i] = v & 0xff, ++v, ++i;
         return;

      case 16:
         /* Generate all 65536 pixel values in order, this includes the 8 bit GA
          * case as we as the 16 bit G case.
          */
         while (i<128)
            buffer[2*i] = (v>>8) & 0xff, buffer[2*i+1] = v & 0xff, ++v, ++i;

         return;

      case 24:
         /* 65535 pixels, but rotate the values. */
         while (i<128)
         {
            /* Three bytes per pixel, r, g, b, make b by r^g */
            buffer[3*i+0] = (v >> 8) & 0xff;
            buffer[3*i+1] = v & 0xff;
            buffer[3*i+2] = ((v >> 8) ^ v) & 0xff;
            ++v;
            ++i;
         }

         return;

      case 32:
         /* 65535 pixels, r, g, b, a; just replicate */
         while (i<128)
         {
            buffer[4*i+0] = (v >> 8) & 0xff;
            buffer[4*i+1] = v & 0xff;
            buffer[4*i+2] = (v >> 8) & 0xff;
            buffer[4*i+3] = v & 0xff;
            ++v;
            ++i;
         }

         return;

      case 48:
         /* y is maximum 2047, giving 4x65536 pixels, make 'r' increase by 1 at
          * each pixel, g increase by 257 (0x101) and 'b' by 0x1111:
          */
         while (i<128)
         {
            png_uint_32 t = v++;
            buffer[6*i+0] = (t >> 8) & 0xff;
            buffer[6*i+1] = t & 0xff;
            t *= 257;
            buffer[6*i+2] = (t >> 8) & 0xff;
            buffer[6*i+3] = t & 0xff;
            t *= 17;
            buffer[6*i+4] = (t >> 8) & 0xff;
            buffer[6*i+5] = t & 0xff;
            ++i;
         }

         return;

      case 64:
         /* As above in the 32 bit case. */
         while (i<128)
         {
            png_uint_32 t = v++;
            buffer[8*i+0] = (t >> 8) & 0xff;
            buffer[8*i+1] = t & 0xff;
            buffer[8*i+4] = (t >> 8) & 0xff;
            buffer[8*i+5] = t & 0xff;
            t *= 257;
            buffer[8*i+2] = (t >> 8) & 0xff;
            buffer[8*i+3] = t & 0xff;
            buffer[8*i+6] = (t >> 8) & 0xff;
            buffer[8*i+7] = t & 0xff;
            ++i;
         }
         return;

      default:
         break;
   }

   png_error(pp, "internal error");
}

/* This is just to do the right cast - could be changed to a function to check
 * 'bd' but there isn't much point.
 */
#define DEPTH(bd) ((png_byte)(1U << (bd)))

static void
make_standard_image(png_store* PNG_CONST ps, png_byte PNG_CONST colour_type,
    png_byte PNG_CONST bit_depth, int interlace_type, png_const_charp name)
{
   context(ps, fault);

   Try
   {
      png_infop pi;
      png_structp pp = set_store_for_write(ps, &pi, name);
      png_uint_32 h;

      /* In the event of a problem return control to the Catch statement below
       * to do the clean up - it is not possible to 'return' directly from a Try
       * block.
       */
      if (pp == NULL)
         Throw ps;

      h = standard_height(pp, colour_type, bit_depth);

      png_set_IHDR(pp, pi, standard_width(pp, colour_type, bit_depth), h,
         bit_depth, colour_type, interlace_type,
         PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);

      if (colour_type == 3) /* palette */
      {
         unsigned int i = 0;
         png_color pal[256];

         do
            pal[i].red = pal[i].green = pal[i].blue = (png_byte)i;
         while(++i < 256U);

         png_set_PLTE(pp, pi, pal, 256);
      }

      png_write_info(pp, pi);

      if (png_get_rowbytes(pp, pi) !=
          standard_rowsize(pp, colour_type, bit_depth))
         png_error(pp, "row size incorrect");

      else
      {
         /* Somewhat confusingly this must be called *after* png_write_info
          * because, if it is called before, the information in *pp has not been
          * updated to reflect the interlaced image.
          */
         int npasses = png_set_interlace_handling(pp);
         int pass;

         if (npasses != npasses_from_interlace_type(pp, interlace_type))
            png_error(pp, "write: png_set_interlace_handling failed");

         for (pass=1; pass<=npasses; ++pass)
         {
            png_uint_32 y;

            for (y=0; y<h; ++y)
            {
               png_byte buffer[STD_ROWMAX];

               standard_row(pp, buffer, colour_type, bit_depth, y);
               png_write_row(pp, buffer);
            }
         }
      }

      png_write_end(pp, pi);

      /* And store this under the appropriate id, then clean up. */
      store_storefile(ps, FILEID(colour_type, bit_depth, interlace_type));

      store_write_reset(ps);
   }

   Catch(fault)
   {
      /* Use the png_store retuned by the exception, this may help the compiler
       * because 'ps' is not used in this branch of the setjmp.  Note that fault
       * and ps will always be the same value.
       */
      store_write_reset(fault);
   }
}

static void
make_standard(png_store* PNG_CONST ps, png_byte PNG_CONST colour_type, int bdlo,
    int PNG_CONST bdhi)
{
   for (; bdlo <= bdhi; ++bdlo)
   {
      int interlace_type;

      for (interlace_type = PNG_INTERLACE_NONE;
           interlace_type < PNG_INTERLACE_LAST; ++interlace_type)
      {
         char name[FILE_NAME_SIZE];

         standard_name(name, sizeof name, 0, colour_type, bdlo, interlace_type);
         make_standard_image(ps, colour_type, DEPTH(bdlo), interlace_type,
            name);
      }
   }
}

static void
make_standard_images(png_store *ps)
{
   /* This is in case of errors. */
   safecat(ps->test, sizeof ps->test, 0, "make standard images");

   /* Arguments are colour_type, low bit depth, high bit depth
    */
   make_standard(ps, 0, 0, WRITE_BDHI);
   make_standard(ps, 2, 3, WRITE_BDHI);
   make_standard(ps, 3, 0, 3/*palette: max 8 bits*/);
   make_standard(ps, 4, 3, WRITE_BDHI);
   make_standard(ps, 6, 3, WRITE_BDHI);
}

/* Tests - individual test cases */
/* Like 'make_standard' but errors are deliberately introduced into the calls
 * to ensure that they get detected - it should not be possible to write an
 * invalid image with libpng!
 */
static void
sBIT0_error_fn(png_structp pp, png_infop pi)
{
   /* 0 is invalid... */
   png_color_8 bad;
   bad.red = bad.green = bad.blue = bad.gray = bad.alpha = 0;
   png_set_sBIT(pp, pi, &bad);
}

static void
sBIT_error_fn(png_structp pp, png_infop pi)
{
   png_byte bit_depth;
   png_color_8 bad;

   if (png_get_color_type(pp, pi) == PNG_COLOR_TYPE_PALETTE)
      bit_depth = 8;

   else
      bit_depth = png_get_bit_depth(pp, pi);

   /* Now we know the bit depth we can easily generate an invalid sBIT entry */
   bad.red = bad.green = bad.blue = bad.gray = bad.alpha =
      (png_byte)(bit_depth+1);
   png_set_sBIT(pp, pi, &bad);
}

static PNG_CONST struct
{
   void          (*fn)(png_structp, png_infop);
   PNG_CONST char *msg;
   unsigned int    warning :1; /* the error is a warning... */
} error_test[] =
    {
       { sBIT0_error_fn, "sBIT(0): failed to detect error", 1 },
       { sBIT_error_fn, "sBIT(too big): failed to detect error", 1 },
    };

static void
make_error(png_store* ps, png_byte PNG_CONST colour_type, png_byte bit_depth,
    int interlace_type, int test, png_const_charp name)
{
   context(ps, fault);

   Try
   {
      png_structp pp;
      png_infop pi;

      pp = set_store_for_write(ps, &pi, name);

      if (pp == NULL)
         Throw ps;

      png_set_IHDR(pp, pi, standard_width(pp, colour_type, bit_depth),
         standard_height(pp, colour_type, bit_depth), bit_depth, colour_type,
         interlace_type, PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);

      if (colour_type == 3) /* palette */
      {
         unsigned int i = 0;
         png_color pal[256];

         do
            pal[i].red = pal[i].green = pal[i].blue = (png_byte)i;
         while(++i < 256U);

         png_set_PLTE(pp, pi, pal, 256);
      }

      /* Time for a few errors, these are in various optional chunks, the
       * standard tests test the standard chunks pretty well.
       */
      Try
      {
         /* Expect this to throw: */
         ps->expect_error = !error_test[test].warning;
         ps->expect_warning = error_test[test].warning;
         ps->saw_warning = 0;
         error_test[test].fn(pp, pi);

         /* Normally the error is only detected here: */
         png_write_info(pp, pi);

         /* And handle the case where it was only a warning: */
         if (ps->expect_warning && ps->saw_warning)
            Throw ps;

         /* If we get here there is a problem, we have success - no error or
          * no warning - when we shouldn't have success.  Log an error.
          */
         store_log(ps, pp, error_test[test].msg, 1/*error*/);
      }

      Catch (fault)
         ps = fault; /* expected exit, make sure ps is not clobbered */

      /* And clear these flags */
      ps->expect_error = 0;
      ps->expect_warning = 0;

      /* Now write the whole image, just to make sure that the detected, or
       * undetected, errro has not created problems inside libpng.
       */
      if (png_get_rowbytes(pp, pi) !=
          standard_rowsize(pp, colour_type, bit_depth))
         png_error(pp, "row size incorrect");

      else
      {
         png_uint_32 h = standard_height(pp, colour_type, bit_depth);
         int npasses = png_set_interlace_handling(pp);
         int pass;

         if (npasses != npasses_from_interlace_type(pp, interlace_type))
            png_error(pp, "write: png_set_interlace_handling failed");

         for (pass=1; pass<=npasses; ++pass)
         {
            png_uint_32 y;
         
            for (y=0; y<h; ++y)
            {
               png_byte buffer[STD_ROWMAX];

               standard_row(pp, buffer, colour_type, bit_depth, y);
               png_write_row(pp, buffer);
            }
         }
      }

      png_write_end(pp, pi);

      /* The following deletes the file that was just written. */
      store_write_reset(ps);
   }

   Catch(fault)
   {
      store_write_reset(fault);
   }
}

static int
make_errors(png_modifier* PNG_CONST pm, png_byte PNG_CONST colour_type,
    int bdlo, int PNG_CONST bdhi)
{
   for (; bdlo <= bdhi; ++bdlo)
   {
      int interlace_type;

      for (interlace_type = PNG_INTERLACE_NONE;
           interlace_type < PNG_INTERLACE_LAST; ++interlace_type)
      {
         unsigned int test;
         char name[FILE_NAME_SIZE];

         standard_name(name, sizeof name, 0, colour_type, bdlo, interlace_type);

         for (test=0; test<(sizeof error_test)/(sizeof error_test[0]); ++test)
         {
            make_error(&pm->this, colour_type, DEPTH(bdlo), interlace_type,
               test, name);

            if (fail(pm))
               return 0;
         }
      }
   }

   return 1; /* keep going */
}

static void
perform_error_test(png_modifier *pm)
{
   /* Need to do this here because we just write in this test. */
   safecat(pm->this.test, sizeof pm->this.test, 0, "error test");

   if (!make_errors(pm, 0, 0, WRITE_BDHI))
      return;

   if (!make_errors(pm, 2, 3, WRITE_BDHI))
      return;

   if (!make_errors(pm, 3, 0, 3))
      return;

   if (!make_errors(pm, 4, 3, WRITE_BDHI))
      return;

   if (!make_errors(pm, 6, 3, WRITE_BDHI))
      return;
}

/* Because we want to use the same code in both the progressive reader and the
 * sequential reader it is necessary to deal with the fact that the progressive
 * reader callbacks only have one parameter (png_get_progressive_ptr()), so this
 * must contain all the test parameters and all the local variables directly
 * accessible to the sequential reader implementation.
 *
 * The techinque adopted is to reinvent part of what Dijkstra termed a
 * 'display'; an array of pointers to the stack frames of enclosing functions so
 * that a nested function definition can access the local (C auto) variables of
 * the functions that contain its definition.  In fact C provides the first
 * pointer (the local variables - the stack frame pointer) and the last (the
 * global variables - the BCPL global vector typically implemented as global
 * addresses), this code requires one more pointer to make the display - the
 * local variables (and function call parameters) of the function that actually
 * invokes either the progressive or sequential reader.
 *
 * Perhaps confusingly this technique is confounded with classes - the
 * 'standard_display' defined here is sub-classed as the 'gamma_display' below.
 * A gamma_display is a standard_display, taking advantage of the ANSI-C
 * requirement that the pointer to the first member of a structure must be the
 * same as the pointer to the structure.  This allows us to reuse standard_
 * functions in the gamma test code; something that could not be done with
 * nested funtions!
 */
typedef struct standard_display
{
   png_store*  ps;             /* Test parameters (passed to the function) */
   png_byte    colour_type;
   png_byte    bit_depth;
   int         interlace_type;
   png_uint_32 id;             /* Calculated file ID */
   png_uint_32 w;              /* Width of image */
   png_uint_32 h;              /* Height of image */
   int         npasses;        /* Number of interlaced passes */
   size_t      cbRow;          /* Bytes in a row of the output image. */
} standard_display;

static void
standard_display_init(standard_display *dp, png_store* ps, png_byte colour_type,
   png_byte bit_depth, int interlace_type)
{
   dp->ps = ps;
   dp->colour_type = colour_type;
   dp->bit_depth = bit_depth;
   dp->interlace_type = interlace_type;
   dp->id = FILEID(colour_type, bit_depth, interlace_type);
   dp->w = 0;
   dp->h = 0;
   dp->npasses = 0;
   dp->cbRow = 0;
}

/* By passing a 'standard_display' the progressive callbacks can be used
 * directly by the sequential code, the functions suffixed _imp are the
 * implementations, the functions without the suffix are the callbacks.
 *
 * The code for the info callback is split into two because this callback calls
 * png_read_update_info or png_start_read_image and what gets called depends on
 * whether the info needs updating (we want to test both calls in pngvalid.)
 */
static void
standard_info_part1(standard_display *dp, png_structp pp, png_infop pi)
{
   if (png_get_bit_depth(pp, pi) != dp->bit_depth)
      png_error(pp, "validate: bit depth changed");

   if (png_get_color_type(pp, pi) != dp->colour_type)
      png_error(pp, "validate: color type changed");

   if (png_get_filter_type(pp, pi) != PNG_FILTER_TYPE_BASE)
      png_error(pp, "validate: filter type changed");

   if (png_get_interlace_type(pp, pi) != dp->interlace_type)
      png_error(pp, "validate: interlacing changed");

   if (png_get_compression_type(pp, pi) != PNG_COMPRESSION_TYPE_BASE)
      png_error(pp, "validate: compression type changed");

   dp->w = png_get_image_width(pp, pi);

   if (dp->w != standard_width(pp, dp->colour_type, dp->bit_depth))
      png_error(pp, "validate: image width changed");

   dp->h = png_get_image_height(pp, pi);

   if (dp->h != standard_height(pp, dp->colour_type, dp->bit_depth))
      png_error(pp, "validate: image height changed");

   /* Important: this is validating the value *before* any transforms have been
    * put in place.  It doesn't matter for the standard tests, where there are
    * no transforms, it does for other tests where rowbytes may change after
    * png_read_update_info.
    */
   if (png_get_rowbytes(pp, pi) != 
       standard_rowsize(pp, dp->colour_type, dp->bit_depth))
      png_error(pp, "validate: row size changed");

   if (dp->colour_type == 3) /* palette */
   {
      png_colorp pal;
      int num;

      /* This could be passed in but isn't - the values set above when the
       * standard images were made are just repeated here.
       */
      if (png_get_PLTE(pp, pi, &pal, &num) & PNG_INFO_PLTE)
      {
         int i;

         if (num != 256)
            png_error(pp, "validate: color type 3 PLTE chunk size changed");

         for (i=0; i<num; ++i)
            if (pal[i].red != i || pal[i].green != i || pal[i].blue != i)
               png_error(pp, "validate: color type 3 PLTE chunk changed");
      }

      else
         png_error(pp, "validate: missing PLTE with color type 3");
   }

   /* Read the number of passes - expected to match the value used when
    * creating the image (interlaced or not).  This has the side effect of
    * turning ono interlace handling.
    */
   dp->npasses = png_set_interlace_handling(pp);

   if (dp->npasses != npasses_from_interlace_type(pp, dp->interlace_type))
      png_error(pp, "validate: file changed interlace type");

   /* Caller calls png_read_update_info or png_start_read_image now, then calls
    * part2.
    */
}

/* This must be called *after* the png_read_update_info call to get the correct
 * 'rowbytes' value, otherwise png_get_rowbytes will refer to the untransformed
 * image.
 */
static void
standard_info_part2(standard_display *dp, png_structp pp, png_infop pi,
    int nImages)
{
   /* Record cbRow now that it can be found. */
   dp->cbRow = png_get_rowbytes(pp, pi);

   /* Then ensure there is enough space for the output image(s). */
   store_ensure_image(dp->ps, pp, nImages * dp->cbRow * dp->h);
}

static void
standard_info_imp(standard_display *dp, png_structp pp, png_infop pi,
    int nImages)
{
   /* Note that the validation routine has the side effect of turning on
    * interlace handling in the subsequent code.
    */
   standard_info_part1(dp, pp, pi);

   /* And the info callback has to call this (or png_read_update_info - see
    * below in the png_modifier code for that variant.
    */
   png_start_read_image(pp);

   /* Validate the height, width and rowbytes plus ensure that sufficient buffer
    * exists for decoding the image.
    */
   standard_info_part2(dp, pp, pi, nImages);
}

static void
standard_info(png_structp pp, png_infop pi)
{
   standard_display *dp = png_get_progressive_ptr(pp);

   /* Call with nImages==1 because the progressive reader can only produce one
    * image.
    */
   standard_info_imp(dp, pp, pi, 1/*only one image*/);
}

static void
progressive_row(png_structp pp, png_bytep new_row, png_uint_32 y, int pass)
{
   UNUSED(pass);

   /* When handling interlacing some rows will be absent in each pass, the
    * callback still gets called, but with a NULL pointer.  We need our own
    * 'cbRow', but we can't call png_get_rowbytes because we got no info
    * structure.
    */
   if (new_row != NULL)
   {
      PNG_CONST standard_display *dp = png_get_progressive_ptr(pp);

      /* Combine the new row into the old: */
      png_progressive_combine_row(pp, dp->ps->image + y * dp->cbRow, new_row);
   }
}

static void
sequential_row(standard_display *dp, png_structp pp, png_infop pi,
    PNG_CONST png_bytep pImage, PNG_CONST png_bytep pDisplay)
{
   PNG_CONST int         npasses = dp->npasses;
   PNG_CONST png_uint_32 h = dp->h;
   PNG_CONST size_t      cbRow = dp->cbRow;
   int pass;

   for (pass=1; pass <= npasses; ++pass)
   {
      png_uint_32 y;
      png_bytep pRow1 = pImage;
      png_bytep pRow2 = pDisplay;

      for (y=0; y<h; ++y)
      {
         png_read_row(pp, pRow1, pRow2);

         if (pRow1 != NULL)
            pRow1 += cbRow;

         if (pRow2 != NULL)
            pRow2 += cbRow;
      }
   }

   /* And finish the read operation (only really necessary if the caller wants
    * to find additional data in png_info from chunks after the last IDAT.)
    */
   png_read_end(pp, pi);
}

static void
standard_row_validate(standard_display *dp, png_structp pp, png_const_bytep row,
   png_const_bytep display, png_uint_32 y)
{
   png_byte std[STD_ROWMAX];
   standard_row(pp, std, dp->colour_type, dp->bit_depth, y);

   /* At the end both the 'read' and 'display' arrays should end up identical.
    * In earlier passes 'read' will be narrow, containing only the columns that
    * were read, and display will be full width but populated with garbage where
    * pixels have not been filled in.
    */
   if (row != NULL && memcmp(std, row, dp->cbRow) != 0)
   {
      char msg[64];
      sprintf(msg, "PNG image row %d changed", y);
      png_error(pp, msg);
   }

   if (display != NULL && memcmp(std, display, dp->cbRow) != 0)
   {
      char msg[64];
      sprintf(msg, "display row %d changed", y);
      png_error(pp, msg);
   }
}

static void
standard_image_validate(standard_display *dp, png_structp pp,
   png_const_bytep pImage, png_const_bytep pDisplay)
{
   png_uint_32 y;

   for (y=0; y<dp->h; ++y)
   {
      standard_row_validate(dp, pp, pImage, pDisplay, y);

      if (pImage != NULL)
         pImage += dp->cbRow;

      if (pDisplay != NULL)
         pDisplay += dp->cbRow;
   }

   /* This avoids false positives if the validation code is never called! */
   dp->ps->validated = 1;
}

static void
standard_end(png_structp pp, png_infop pi)
{
   standard_display *dp = png_get_progressive_ptr(pp);

   UNUSED(pi);

   /* Validate the image - progressive reading only produces one variant for
    * interlaced images.
    */
   standard_image_validate(dp, pp, dp->ps->image, NULL);
}

/* A single test run checking the standard image to ensure it is not damaged. */
static void
standard_test(png_store* PNG_CONST psIn, png_byte PNG_CONST colour_typeIn,
   png_byte PNG_CONST bit_depthIn, int interlace_typeIn)
{
   standard_display d;
   context(psIn, fault);

   /* Set up the display (stack frame) variables from the arguments to the
    * function and initialize the locals that are filled in later.
    */
   standard_display_init(&d, psIn, colour_typeIn, bit_depthIn,
      interlace_typeIn);

   /* Everything is protected by a Try/Catch.  The functions called also
    * typically have local Try/Catch blocks.
    */
   Try
   {
      png_structp pp;
      png_infop pi;

      /* Get a png_struct for writing the image, this will throw an error if it
       * fails, so we don't need to check the result.
       */
      pp = set_store_for_read(d.ps, &pi, d.id,
         d.ps->progressive ? "progressive reader" : "sequential reader");

      /* Introduce the correct read function. */
      if (d.ps->progressive)
      {
         png_set_progressive_read_fn(pp, &d, standard_info, progressive_row,
            standard_end);

         /* Now feed data into the reader until we reach the end: */
         store_progressive_read(d.ps, pp, pi);
      }
      else
      {
         /* Note that this takes the store, not the display. */
         png_set_read_fn(pp, d.ps, store_read);

         /* Check the header values: */
         png_read_info(pp, pi);

         /* The code tests both versions of the images that the sequential
          * reader can produce.
          */
         standard_info_imp(&d, pp, pi, 2/*images*/);

         /* Need the total bytes in the image below; we can't get to this point
          * unless the PNG file values have been checked against the expected
          * values.
          */
         {
            PNG_CONST png_bytep pImage = d.ps->image;
            PNG_CONST png_bytep pDisplay = pImage + d.cbRow * d.h;

            sequential_row(&d, pp, pi, pImage, pDisplay);

            /* After the last pass loop over the rows again to check that the
             * image is correct.
             */
            standard_image_validate(&d, pp, pImage, pDisplay);
         }
      }

      /* Check for validation. */
      if (!d.ps->validated)
         png_error(pp, "image read failed silently");

      /* Successful completion. */
   }

   Catch(fault)
      d.ps = fault; /* make sure this hasn't been clobbered. */

   /* In either case clean up the store. */
   store_read_reset(d.ps);
}

static int
test_standard(png_modifier* PNG_CONST pm, png_byte PNG_CONST colour_type,
    int bdlo, int PNG_CONST bdhi)
{
   for (; bdlo <= bdhi; ++bdlo)
   {
      int interlace_type;

      for (interlace_type = PNG_INTERLACE_NONE;
           interlace_type < PNG_INTERLACE_LAST; ++interlace_type)
      {
         /* Test both sequential and standard readers here. */
         pm->this.progressive = !pm->this.progressive;
         standard_test(&pm->this, colour_type, DEPTH(bdlo), interlace_type);

         if (fail(pm))
            return 0;

         pm->this.progressive = !pm->this.progressive;
         standard_test(&pm->this, colour_type, DEPTH(bdlo), interlace_type);

         if (fail(pm))
            return 0;
      }
   }

   return 1; /*keep going*/
}

static void
perform_standard_test(png_modifier *pm)
{
   /* Test each colour type over the valid range of bit depths (expressed as
    * log2(bit_depth) in turn, stop as soon as any error is detected.
    */
   if (!test_standard(pm, 0, 0, READ_BDHI))
      return;

   if (!test_standard(pm, 2, 3, READ_BDHI))
      return;

   if (!test_standard(pm, 3, 0, 3))
      return;

   if (!test_standard(pm, 4, 3, READ_BDHI))
      return;

   if (!test_standard(pm, 6, 3, READ_BDHI))
      return;
}


/********************************* GAMMA TESTS ********************************/
/* Gamma test images. */
typedef struct gamma_modification
{
   png_modification this;
   png_fixed_point  gamma;
} gamma_modification;

static int
gamma_modify(png_modifier *pm, png_modification *me, int add)
{
   UNUSED(add);
   /* This simply dumps the given gamma value into the buffer. */
   png_save_uint_32(pm->buffer, 4);
   png_save_uint_32(pm->buffer+4, CHUNK_gAMA);
   png_save_uint_32(pm->buffer+8, ((gamma_modification*)me)->gamma);
   return 1;
}

static void
gamma_modification_init(gamma_modification *me, png_modifier *pm, double gamma)
{
   double g;

   modification_init(&me->this);
   me->this.chunk = CHUNK_gAMA;
   me->this.modify_fn = gamma_modify;
   me->this.add = CHUNK_PLTE;
   g = floor(gamma * 100000 + .5);
   me->gamma = (png_fixed_point)g;
   me->this.next = pm->modifications;
   pm->modifications = &me->this;
}

typedef struct srgb_modification
{
   png_modification this;
   png_byte         intent;
} srgb_modification;

static int
srgb_modify(png_modifier *pm, png_modification *me, int add)
{
   UNUSED(add);
   /* As above, ignore add and just make a new chunk */
   png_save_uint_32(pm->buffer, 1);
   png_save_uint_32(pm->buffer+4, CHUNK_sRGB);
   pm->buffer[8] = ((srgb_modification*)me)->intent;
   return 1;
}

static void
srgb_modification_init(srgb_modification *me, png_modifier *pm, png_byte intent)
{
   modification_init(&me->this);
   me->this.chunk = CHUNK_sBIT;

   if (intent <= 3) /* if valid, else *delete* sRGB chunks */
   {
      me->this.modify_fn = srgb_modify;
      me->this.add = CHUNK_PLTE;
      me->intent = intent;
   }

   else
   {
      me->this.modify_fn = 0;
      me->this.add = 0;
      me->intent = 0;
   }

   me->this.next = pm->modifications;
   pm->modifications = &me->this;
}

typedef struct sbit_modification
{
   png_modification this;
   png_byte         sbit;
} sbit_modification;

static int
sbit_modify(png_modifier *pm, png_modification *me, int add)
{
   png_byte sbit = ((sbit_modification*)me)->sbit;
   if (pm->bit_depth > sbit)
   {
      int cb = 0;
      switch (pm->colour_type)
      {
         case 0:
            cb = 1;
            break;

         case 2:
         case 3:
            cb = 3;
            break;

         case 4:
            cb = 2;
            break;

         case 6:
            cb = 4;
            break;

         default:
            png_error(pm->this.pread,
               "unexpected colour type in sBIT modification");
      }

      png_save_uint_32(pm->buffer, cb);
      png_save_uint_32(pm->buffer+4, CHUNK_sBIT);

      while (cb > 0)
         (pm->buffer+8)[--cb] = sbit;

      return 1;
   }
   else if (!add)
   {
      /* Remove the sBIT chunk */
      pm->buffer_count = pm->buffer_position = 0;
      return 1;
   }
   else
      return 0; /* do nothing */
}

static void
sbit_modification_init(sbit_modification *me, png_modifier *pm, png_byte sbit)
{
   modification_init(&me->this);
   me->this.chunk = CHUNK_sBIT;
   me->this.modify_fn = sbit_modify;
   me->this.add = CHUNK_PLTE;
   me->sbit = sbit;
   me->this.next = pm->modifications;
   pm->modifications = &me->this;
}

/* Reader callbacks and implementations, where they differ from the standard
 * ones.
 */
typedef struct gamma_display
{
   standard_display this;

   /* Parameters */
   png_modifier*    pm;
   double           file_gamma;
   double           screen_gamma;
   png_byte         sbit;
   int              threshold_test;
   PNG_CONST char*  name;
   int              speed;
   int              use_input_precision;
   int              strip16;

   /* Local variables */
   double       maxerrout;
   double       maxerrpc;
   double       maxerrabs;
} gamma_display;

static void
gamma_display_init(gamma_display *dp, png_modifier *pm, png_byte colour_type,
    png_byte bit_depth, int interlace_type, double file_gamma,
    double screen_gamma, png_byte sbit, int threshold_test, int speed,
    int use_input_precision, int strip16)
{
   /* Standard fields */
   standard_display_init(&dp->this, &pm->this, colour_type, bit_depth,
      interlace_type);
   
   /* Parameter fields */
   dp->pm = pm;
   dp->file_gamma = file_gamma;
   dp->screen_gamma = screen_gamma;
   dp->sbit = sbit;
   dp->threshold_test = threshold_test;
   dp->speed = speed;
   dp->use_input_precision = use_input_precision;
   dp->strip16 = strip16;

   /* Local variable fields */
   dp->maxerrout = dp->maxerrpc = dp->maxerrabs = 0;
}

static void
gamma_info_imp(gamma_display *dp, png_structp pp, png_infop pi)
{
   /* Reuse the standard stuff as appropriate. */
   standard_info_part1(&dp->this, pp, pi);

   /* If requested strip 16 to 8 bits - this is handled automagically below
    * because the output bit depth is read from the library.  Note that there
    * are interactions with sBIT but, internally, libpng makes sbit at most
    * PNG_MAX_GAMMA_8 when doing the following.
    */
   if (dp->strip16)
#     ifdef PNG_READ_16_TO_8
         png_set_strip_16(pp);
#     else
         png_error(pp, "strip16 (16 to 8 bit conversion) not supported");
#     endif

   png_read_update_info(pp, pi);

   /* Now we may get a different cbRow: */
   standard_info_part2(&dp->this, pp, pi, 1/*images*/);
}

static void
gamma_info(png_structp pp, png_infop pi)
{
   gamma_info_imp(png_get_progressive_ptr(pp), pp, pi);
}

static void
gamma_image_validate(gamma_display *dp, png_structp pp, png_infop pi,
    png_const_bytep pRow)
{
   /* Get some constants derived from the input and output file formats: */
   PNG_CONST png_byte sbit = dp->sbit;
   PNG_CONST double file_gamma = dp->file_gamma;
   PNG_CONST double screen_gamma = dp->screen_gamma;
   PNG_CONST int use_input_precision = dp->use_input_precision;
   PNG_CONST int speed = dp->speed;
   PNG_CONST png_byte in_ct = dp->this.colour_type;
   PNG_CONST png_byte in_bd = dp->this.bit_depth;
   PNG_CONST png_uint_32 w = dp->this.w;
   PNG_CONST png_uint_32 h = dp->this.h;
   PNG_CONST size_t cbRow = dp->this.cbRow;
   PNG_CONST png_byte out_ct = png_get_color_type(pp, pi);
   PNG_CONST png_byte out_bd = png_get_bit_depth(pp, pi);
   PNG_CONST unsigned int outmax = (1U<<out_bd)-1;
   PNG_CONST double maxabs = abserr(dp->pm, out_bd);
   PNG_CONST double maxout = outerr(dp->pm, out_bd);
   PNG_CONST double maxpc = pcerr(dp->pm, out_bd);

   /* There are three sources of error, firstly the quantization in the
    * file encoding, determined by sbit and/or the file depth, secondly
    * the output (screen) gamma and thirdly the output file encoding.
    * Since this API receives the screen and file gamma in double
    * precision it is possible to calculate an exact answer given an input
    * pixel value.  Therefore we assume that the *input* value is exact -
    * sample/maxsample - calculate the corresponding gamma corrected
    * output to the limits of double precision arithmetic and compare with
    * what libpng returns.
    *
    * Since the library must quantise the output to 8 or 16 bits there is
    * a fundamental limit on the accuracy of the output of +/-.5 - this
    * quantisation limit is included in addition to the other limits
    * specified by the paramaters to the API.  (Effectively, add .5
    * everywhere.)
    *
    * The behavior of the 'sbit' paramter is defined by section 12.5
    * (sample depth scaling) of the PNG spec.  That section forces the
    * decoder to assume that the PNG values have been scaled if sBIT is
    * presence:
    *
    *     png-sample = floor( input-sample * (max-out/max-in) + .5 );
    *
    * This means that only a subset of the possible PNG values should
    * appear in the input, however the spec allows the encoder to use a
    * variety of approximations to the above and doesn't require any
    * restriction of the values produced.
    *
    * Nevertheless the spec requires that the upper 'sBIT' bits of the
    * value stored in a PNG file be the original sample bits.
    * Consequently the code below simply scales the top sbit bits by
    * (1<<sbit)-1 to obtain an original sample value.
    *
    * Because there is limited precision in the input it is arguable that
    * an acceptable result is any valid result from input-.5 to input+.5.
    * The basic tests below do not do this, however if
    * 'use_input_precision' is set a subsequent test is performed below.
    */
   PNG_CONST int processing = (fabs(screen_gamma*file_gamma-1) >=
      PNG_GAMMA_THRESHOLD && !dp->threshold_test && !speed && in_ct != 3) ||
      in_bd != out_bd;

   PNG_CONST unsigned int samples_per_pixel = (out_ct & 2U) ? 3U : 1U;

   PNG_CONST double gamma = 1/(file_gamma*screen_gamma); /* Overall */

   double maxerrout = 0, maxerrabs = 0, maxerrpc = 0;
   png_uint_32 y;

   for (y=0; y<h; ++y, pRow += cbRow)
   {
      unsigned int s, x;
      png_byte std[STD_ROWMAX];

      standard_row(pp, std, in_ct, in_bd, y);

      if (processing)
      {
         for (x=0; x<w; ++x) for (s=0; s<samples_per_pixel; ++s)
         {
            /* Input sample values: */
            PNG_CONST unsigned int
               id = sample(std, in_ct, in_bd, x, s);

            PNG_CONST unsigned int
               od = sample(pRow, out_ct, out_bd, x, s);

            PNG_CONST unsigned int
               isbit = id >> (in_bd-sbit);

            double i, sample, encoded_sample, output, encoded_error, error;
            double es_lo, es_hi;

            /* First check on the 'perfect' result obtained from the
             * digitized input value, id, and compare this against the
             * actual digitized result, 'od'.  'i' is the input result
             * in the range 0..1:
             *
             * NOTE: sBIT should be taken into account here but isn't,
             * as described above.
             */
            i = isbit; i /= (1U<<sbit)-1;

            /* Then get the gamma corrected version of 'i' and compare
             * to 'od', any error less than .5 is insignificant - just
             * quantization of the output value to the nearest digital
             * value (nevertheless the error is still recorded - it's
             * interesting ;-)
             */
            encoded_sample = pow(i, gamma) * outmax;
            encoded_error = fabs(od-encoded_sample);

            if (encoded_error > maxerrout)
               maxerrout = encoded_error;

            if (encoded_error < .5+maxout)
               continue;

            /* There may be an error, calculate the actual sample
             * values - unencoded light intensity values.  Note that
             * in practice these are not unencoded because they
             * include a 'viewing correction' to decrease or
             * (normally) increase the perceptual contrast of the
             * image.  There's nothing we can do about this - we don't
             * know what it is - so assume the unencoded value is
             * perceptually linear.
             */
            sample = pow(i, 1/file_gamma); /* In range 0..1 */
            output = od;
            output /= outmax;
            output = pow(output, screen_gamma);

            /* Now we have the numbers for real errors, both absolute
             * values as as a percentage of the correct value (output):
             */
            error = fabs(sample-output);

            if (error > maxerrabs)
               maxerrabs = error;

            /* The following is an attempt to ignore the tendency of
             * quantization to dominate the percentage errors for low
             * output sample values:
             */
            if (sample*maxpc > .5+maxabs)
            {
               double pcerr = error/sample;
               if (pcerr > maxerrpc) maxerrpc = pcerr;
            }

            /* Now calculate the digitization limits for
             * 'encoded_sample' using the 'max' values.  Note that
             * maxout is in the encoded space but maxpc and maxabs are
             * in linear light space.
             *
             * First find the maximum error in linear light space,
             * range 0..1:
             */
            {
               double tmp = sample * maxpc;
               if (tmp < maxabs) tmp = maxabs;

               /* Low bound - the minimum of the three: */
               es_lo = encoded_sample - maxout;
               if (es_lo > 0 && sample-tmp > 0)
               {
                  double l = outmax * pow(sample-tmp, 1/screen_gamma);
                  if (l < es_lo) es_lo = l;
               }
               else
                  es_lo = 0;

               es_hi = encoded_sample + maxout;
               if (es_hi < outmax && sample+tmp < 1)
               {
                  double h = outmax * pow(sample+tmp, 1/screen_gamma);
                  if (h > es_hi) es_hi = h;
               }
               else
                  es_hi = outmax;
            }

            /* The primary test is that the final encoded value
             * returned by the library should be between the two limits
             * (inclusive) that were calculated above.  At this point
             * quantization of the output must be taken into account.
             */
            if (od+.5 < es_lo || od-.5 > es_hi)
            {
               /* There has been an error in processing. */
               double is_lo, is_hi;

               if (use_input_precision)
               {
                  /* Ok, something is wrong - this actually happens in
                   * current libpng sbit processing.  Assume that the
                   * input value (id, adjusted for sbit) can be
                   * anywhere between value-.5 and value+.5 - quite a
                   * large range if sbit is low.
                   */
                  double tmp = (isbit - .5)/((1U<<sbit)-1);

                  if (tmp > 0)
                  {
                     is_lo = outmax * pow(tmp, gamma) - maxout;
                     if (is_lo < 0) is_lo = 0;
                  }

                  else
                     is_lo = 0;

                  tmp = (isbit + .5)/((1U<<sbit)-1);

                  if (tmp < 1)
                  {
                     is_hi = outmax * pow(tmp, gamma) + maxout;
                     if (is_hi > outmax) is_hi = outmax;
                  }

                  else
                     is_hi = outmax;

                  if (!(od+.5 < is_lo || od-.5 > is_hi))
                     continue;
               }
               else
                  is_lo = es_lo, is_hi = es_hi;

               {
                  char msg[256];
                  sprintf(msg,
                   "error: %.3f; %u{%u;%u} -> %u not %.2f (%.1f-%.1f)",
                     od-encoded_sample, id, sbit, isbit, od,
                     encoded_sample, is_lo, is_hi);
                  png_warning(pp, msg);
               }
            }
         }
      }

      else if (!speed && memcmp(std, pRow, cbRow) != 0)
      {
         char msg[64];
         /* No transform is expected on the threshold tests. */
         sprintf(msg, "gamma: below threshold row %d changed", y);
         png_error(pp, msg);
      }
   } /* row (y) loop */

   dp->maxerrout = maxerrout;
   dp->maxerrabs = maxerrabs;
   dp->maxerrpc = maxerrpc;
   dp->this.ps->validated = 1;
}

static void
gamma_end(png_structp pp, png_infop pi)
{
   gamma_display *dp = png_get_progressive_ptr(pp);

   gamma_image_validate(dp, pp, pi, dp->this.ps->image);
}

/* A single test run checking a gamma transformation.
 *
 * maxabs: maximum absolute error as a fraction
 * maxout: maximum output error in the output units
 * maxpc:  maximum percentage error (as a percentage)
 */
static void
gamma_test(png_modifier *pmIn, PNG_CONST png_byte colour_typeIn,
    PNG_CONST png_byte bit_depthIn, PNG_CONST int interlace_typeIn,
    PNG_CONST double file_gammaIn, PNG_CONST double screen_gammaIn,
    PNG_CONST png_byte sbitIn, PNG_CONST int threshold_testIn,
    PNG_CONST char *name, PNG_CONST int speedIn,
    PNG_CONST int use_input_precisionIn, PNG_CONST int strip16In)
{
   gamma_display d;
   context(&pmIn->this, fault);

   gamma_display_init(&d, pmIn, colour_typeIn, bit_depthIn, interlace_typeIn,
      file_gammaIn, screen_gammaIn, sbitIn, threshold_testIn, speedIn,
      use_input_precisionIn, strip16In);

   Try
   {
      png_structp pp;
      png_infop pi;
      gamma_modification gamma_mod;
      srgb_modification srgb_mod;
      sbit_modification sbit_mod;

      /* Make an appropriate modifier to set the PNG file gamma to the
       * given gamma value and the sBIT chunk to the given precision.
       */
      d.pm->modifications = NULL;
      gamma_modification_init(&gamma_mod, d.pm, d.file_gamma);
      srgb_modification_init(&srgb_mod, d.pm, 127/*delete*/);
      sbit_modification_init(&sbit_mod, d.pm, d.sbit);

      modification_reset(d.pm->modifications);

      /* Get a png_struct for writing the image. */
      pp = set_modifier_for_read(d.pm, &pi, d.this.id, name);

      /* Set up gamma processing. */
#ifdef PNG_FLOATING_POINT_SUPPORTED
      png_set_gamma(pp, d.screen_gamma, d.file_gamma);
#else
      {
         png_fixed_point s = floor(d.screen_gamma*100000+.5);
         png_fixed_point f = floor(d.file_gamma*100000+.5);
         png_set_gamma_fixed(pp, s, f);
      }
#endif

      /* Introduce the correct read function. */
      if (d.pm->this.progressive)
      {
         /* Share the row function with the standard implementation. */
         png_set_progressive_read_fn(pp, &d, gamma_info, progressive_row,
            gamma_end);

         /* Now feed data into the reader until we reach the end: */
         modifier_progressive_read(d.pm, pp, pi);
      }
      else
      {
         /* modifier_read expects a png_modifier* */
         png_set_read_fn(pp, d.pm, modifier_read);

         /* Check the header values: */
         png_read_info(pp, pi);

         /* Process the 'info' requirements. only one image is generated */
         gamma_info_imp(&d, pp, pi);

         sequential_row(&d.this, pp, pi, NULL, d.this.ps->image);

         gamma_image_validate(&d, pp, pi, d.this.ps->image);
      }

      modifier_reset(d.pm);

      if (d.pm->log && !d.threshold_test && !d.speed)
         fprintf(stderr, "%d bit %s %s: max error %f (%.2g, %2g%%)\n",
            d.this.bit_depth, colour_types[d.this.colour_type], d.name,
            d.maxerrout, d.maxerrabs, 100*d.maxerrpc);

      /* Log the summary values too. */
      if (d.this.colour_type == 0 || d.this.colour_type == 4)
      {
         switch (d.this.bit_depth)
         {
         case 1:
            break;

         case 2:
            if (d.maxerrout > d.pm->error_gray_2)
               d.pm->error_gray_2 = d.maxerrout;
            break;

         case 4:
            if (d.maxerrout > d.pm->error_gray_4)
               d.pm->error_gray_4 = d.maxerrout;
            break;

         case 8:
            if (d.maxerrout > d.pm->error_gray_8)
               d.pm->error_gray_8 = d.maxerrout;
            break;

         case 16:
            if (d.maxerrout > d.pm->error_gray_16)
               d.pm->error_gray_16 = d.maxerrout;
            break;

         default:
            png_error(pp, "bad bit depth (internal: 1)");
         }
      }

      else if (d.this.colour_type == 2 || d.this.colour_type == 6)
      {
         switch (d.this.bit_depth)
         {
         case 8:
            if (d.maxerrout > d.pm->error_color_8)
               d.pm->error_color_8 = d.maxerrout;

            break;

         case 16:
            if (d.maxerrout > d.pm->error_color_16)
               d.pm->error_color_16 = d.maxerrout;

            break;

         default:
            png_error(pp, "bad bit depth (internal: 2)");
         }
      }
   }

   Catch(fault)
      modifier_reset((png_modifier*)fault);
}

static void gamma_threshold_test(png_modifier *pm, png_byte colour_type,
    png_byte bit_depth, int interlace_type, double file_gamma,
    double screen_gamma)
{
   size_t pos = 0;
   char name[64];
   pos = safecat(name, sizeof name, pos, "threshold ");
   pos = safecatd(name, sizeof name, pos, file_gamma, 3);
   pos = safecat(name, sizeof name, pos, "/");
   pos = safecatd(name, sizeof name, pos, screen_gamma, 3);

   (void)gamma_test(pm, colour_type, bit_depth, interlace_type, file_gamma,
      screen_gamma, bit_depth, 1, name, 0/*speed*/, 0/*no input precision*/,
      0/*no strip16*/);
}

static void
perform_gamma_threshold_tests(png_modifier *pm)
{
   png_byte colour_type = 0;
   png_byte bit_depth = 0;

   while (next_format(&colour_type, &bit_depth))
   {
      double gamma = 1.0;
      while (gamma >= .4)
      {
         /* There's little point testing the interlacing vs non-interlacing,
          * but this can be set from the command line.
          */
         gamma_threshold_test(pm, colour_type, bit_depth, pm->interlace_type,
            gamma, 1/gamma);
         gamma *= .95;
      }

      /* And a special test for sRGB */
      gamma_threshold_test(pm, colour_type, bit_depth, pm->interlace_type,
          .45455, 2.2);

      if (fail(pm))
         return;
   }
}

static void gamma_transform_test(png_modifier *pm,
   PNG_CONST png_byte colour_type, PNG_CONST png_byte bit_depth,
   PNG_CONST int interlace_type, PNG_CONST double file_gamma,
   PNG_CONST double screen_gamma, PNG_CONST png_byte sbit, PNG_CONST int speed,
   PNG_CONST int use_input_precision, PNG_CONST int strip16)
{
   size_t pos = 0;
   char name[64];

   if (sbit != bit_depth)
   {
      pos = safecat(name, sizeof name, pos, "sbit(");
      pos = safecatn(name, sizeof name, pos, sbit);
      pos = safecat(name, sizeof name, pos, ") ");
   }

   else
      pos = safecat(name, sizeof name, pos, "gamma ");

   if (strip16)
      pos = safecat(name, sizeof name, pos, "16to8 ");
   pos = safecatd(name, sizeof name, pos, file_gamma, 3);
   pos = safecat(name, sizeof name, pos, "->");
   pos = safecatd(name, sizeof name, pos, screen_gamma, 3);

   gamma_test(pm, colour_type, bit_depth, interlace_type, file_gamma,
      screen_gamma, sbit, 0, name, speed, use_input_precision, strip16);
}

static void perform_gamma_transform_tests(png_modifier *pm, int speed)
{
   png_byte colour_type = 0;
   png_byte bit_depth = 0;

   /* Ignore palette images - the gamma correction happens on the palette entry,
    * haven't got the tests for this yet.
    */
   while (next_format(&colour_type, &bit_depth)) if (colour_type != 3)
   {
      unsigned int i, j;

      for (i=0; i<pm->ngammas; ++i) for (j=0; j<pm->ngammas; ++j) if (i != j)
      {
         gamma_transform_test(pm, colour_type, bit_depth, pm->interlace_type,
            1/pm->gammas[i], pm->gammas[j], bit_depth, speed,
            pm->use_input_precision, 0/*do not strip16*/);

         if (fail(pm))
            return;
      }
   }
}

static void perform_gamma_sbit_tests(png_modifier *pm, int speed)
{
   png_byte sbit;

   /* The only interesting cases are colour and grayscale, alpha is ignored here
    * for overall speed.  Only bit depths 8 and 16 are tested.
    */
   for (sbit=pm->sbitlow; sbit<(1<<READ_BDHI); ++sbit)
   {
      unsigned int i, j;

      for (i=0; i<pm->ngammas; ++i)
      {
         for (j=0; j<pm->ngammas; ++j)
         {
            if (i != j)
            {
               if (sbit < 8)
               {
                  gamma_transform_test(pm, 0, 8, pm->interlace_type,
                      1/pm->gammas[i], pm->gammas[j], sbit, speed,
                      pm->use_input_precision_sbit, 0/*strip16*/);

                  if (fail(pm))
                     return;

                  gamma_transform_test(pm, 2, 8, pm->interlace_type,
                      1/pm->gammas[i], pm->gammas[j], sbit, speed,
                      pm->use_input_precision_sbit, 0/*strip16*/);

                  if (fail(pm))
                     return;
               }

#ifdef DO_16BIT
               gamma_transform_test(pm, 0, 16, pm->interlace_type,
                   1/pm->gammas[i], pm->gammas[j], sbit, speed,
                   pm->use_input_precision_sbit, 0/*strip16*/);

               if (fail(pm))
                  return;

               gamma_transform_test(pm, 2, 16, pm->interlace_type,
                   1/pm->gammas[i], pm->gammas[j], sbit, speed,
                   pm->use_input_precision_sbit, 0/*strip16*/);

               if (fail(pm))
                  return;
#endif
            }
         }
      }
   }
}

/* Note that this requires a 16 bit source image but produces 8 bit output, so
 * we only need the 16bit write support.
 */
#ifdef PNG_16_TO_8_SUPPORTED
static void perform_gamma_strip16_tests(png_modifier *pm, int speed)
{
#  ifndef PNG_MAX_GAMMA_8
#     define PNG_MAX_GAMMA_8 11
#  endif
   /* Include the alpha cases here, not that sbit matches the internal value
    * used by the library - otherwise we will get spurious errors from the
    * internal sbit style approximation.
    *
    * The threshold test is here because otherwise the 16 to 8 conversion will
    * proceed *without* gamma correction, and the tests above will fail (but not
    * by much) - this could be fixed, it only appears with the -g option.
    */
   unsigned int i, j;
   for (i=0; i<pm->ngammas; ++i)
   {
      for (j=0; j<pm->ngammas; ++j)
      {
         if (i != j &&
             fabs(pm->gammas[j]/pm->gammas[i]-1) >= PNG_GAMMA_THRESHOLD)
         {
            gamma_transform_test(pm, 0, 16, pm->interlace_type, 1/pm->gammas[i],
                pm->gammas[j], PNG_MAX_GAMMA_8, speed,
                pm->use_input_precision_16to8, 1/*strip16*/);

            if (fail(pm))
               return;

            gamma_transform_test(pm, 2, 16, pm->interlace_type, 1/pm->gammas[i],
                pm->gammas[j], PNG_MAX_GAMMA_8, speed,
                pm->use_input_precision_16to8, 1/*strip16*/);

            if (fail(pm))
               return;

            gamma_transform_test(pm, 4, 16, pm->interlace_type, 1/pm->gammas[i],
                pm->gammas[j], PNG_MAX_GAMMA_8, speed,
                pm->use_input_precision_16to8, 1/*strip16*/);

            if (fail(pm))
               return;

            gamma_transform_test(pm, 6, 16, pm->interlace_type, 1/pm->gammas[i],
                pm->gammas[j], PNG_MAX_GAMMA_8, speed,
                pm->use_input_precision_16to8, 1/*strip16*/);

            if (fail(pm))
               return;
         }
      }
   }
}
#endif /* 16 to 8 bit conversion */

static void
perform_gamma_test(png_modifier *pm, int speed, int summary)
{
   /* First some arbitrary no-transform tests: */
   if (!speed)
   {
      perform_gamma_threshold_tests(pm);

      if (fail(pm))
         return;
   }

   /* Now some real transforms. */
   perform_gamma_transform_tests(pm, speed);

   if (summary)
   {
      printf("Gamma correction error summary (output value error):\n");
      printf("  2 bit gray:  %.5f\n", pm->error_gray_2);
      printf("  4 bit gray:  %.5f\n", pm->error_gray_4);
      printf("  8 bit gray:  %.5f\n", pm->error_gray_8);
      printf("  8 bit color: %.5f\n", pm->error_color_8);
#ifdef DO_16BIT
      printf(" 16 bit gray:  %.5f\n", pm->error_gray_16);
      printf(" 16 bit color: %.5f\n", pm->error_color_16);
#endif
   }

   /* The sbit tests produce much larger errors: */
   pm->error_gray_2 = pm->error_gray_4 = pm->error_gray_8 = pm->error_gray_16 =
   pm->error_color_8 = pm->error_color_16 = 0;
   perform_gamma_sbit_tests(pm, speed);

   if (summary)
   {
      printf("Gamma correction with sBIT:\n");

      if (pm->sbitlow < 8U)
      {
         printf("  2 bit gray:  %.5f\n", pm->error_gray_2);
         printf("  4 bit gray:  %.5f\n", pm->error_gray_4);
         printf("  8 bit gray:  %.5f\n", pm->error_gray_8);
         printf("  8 bit color: %.5f\n", pm->error_color_8);
      }

#ifdef DO_16BIT
      printf(" 16 bit gray:  %.5f\n", pm->error_gray_16);
      printf(" 16 bit color: %.5f\n", pm->error_color_16);
#endif
   }

#ifdef PNG_16_TO_8_SUPPORTED
   /* The 16 to 8 bit strip operations: */
   pm->error_gray_2 = pm->error_gray_4 = pm->error_gray_8 = pm->error_gray_16 =
   pm->error_color_8 = pm->error_color_16 = 0;
   perform_gamma_strip16_tests(pm, speed);
   if (summary)
   {
      printf("Gamma correction with 16 to 8 bit reduction:\n");
      printf(" 16 bit gray:  %.5f\n", pm->error_gray_16);
      printf(" 16 bit color: %.5f\n", pm->error_color_16);
   }
#endif
}

/* main program */
int main(int argc, PNG_CONST char **argv)
{
   volatile int summary = 1;    /* Print the error sumamry at the end */

   /* Create the given output file on success: */
   PNG_CONST char *volatile touch = NULL;

   /* This is an array of standard gamma values (believe it or not I've seen
    * every one of these mentioned somewhere.)
    *
    * In the following list the most useful values are first!
    */
   static double
      gammas[]={2.2, 1.0, 2.2/1.45, 1.8, 1.5, 2.4, 2.5, 2.62, 2.9};

   png_modifier pm;
   context(&pm.this, fault);

   modifier_init(&pm);

   /* Preallocate the image buffer, because we know how big it needs to be,
    * note that, for testing purposes, it is deliberately mis-aligned.
    */
   pm.this.image = malloc(2*STD_IMAGEMAX+1);
   if (pm.this.image != NULL)
   {
      /* Ignore OOM at this point - the 'ensure' routine above will allocate the
       * array appropriately.
       */
      ++(pm.this.image);
      pm.this.cb_image = 2*STD_IMAGEMAX;
   }

   /* Default to error on warning: */
   pm.this.treat_warnings_as_errors = 1;

   /* Store the test gammas */
   pm.gammas = gammas;
   pm.ngammas = 3U; /* for speed */
   pm.sbitlow = 8U; /* because libpng doesn't do sBIT below 8! */
   pm.use_input_precision_16to8 = 1U; /* Because of the way libpng does it */

   /* Some default values (set the behavior for 'make check' here) */
   pm.maxout8 = .1;     /* Arithmetic error in *encoded* value */
   pm.maxabs8 = .00005; /* 1/20000 */
   pm.maxpc8 = .499;    /* I.e. .499% fractional error */
   pm.maxout16 = .499;  /* Error in *encoded* value */
   pm.maxabs16 = .00005;/* 1/20000 */
   /* NOTE: this is a reasonable perceptual limit, we assume that humans can
    * perceive light level differences of 1% over a 100:1 range, so we need to
    * maintain 1 in 10000 accuracy (in linear light space), this is what the
    * following guarantees.  It also allows significantly higher errors at
    * higher 16 bit values, which is important for performance.  The actual
    * maximum 16 bit error is about +/-1.9 in the fixed point implementation but
    * this is only allowed for values >38149 by the following:
    */
   pm.maxpc16 = .005;   /* I.e. 1/200% - 1/20000 */

   /* Now parse the command line options. */
   while (--argc >= 1)
   {
      if (strcmp(*++argv, "-v") == 0)
         pm.this.verbose = 1;

      else if (strcmp(*argv, "-l") == 0)
         pm.log = 1;

      else if (strcmp(*argv, "-q") == 0)
         summary = pm.this.verbose = pm.log = 0;

      else if (strcmp(*argv, "-g") == 0)
         pm.ngammas = (sizeof gammas)/(sizeof gammas[0]);

      else if (strcmp(*argv, "-w") == 0)
         pm.this.treat_warnings_as_errors = 0;

      else if (strcmp(*argv, "--speed") == 0)
         pm.this.speed = 1, pm.ngammas = (sizeof gammas)/(sizeof gammas[0]);

      else if (strcmp(*argv, "--nogamma") == 0)
         pm.ngammas = 0;

      else if (strcmp(*argv, "--progressive-read") == 0)
         pm.this.progressive = 1;

      else if (strcmp(*argv, "--interlace") == 0)
         pm.interlace_type = PNG_INTERLACE_ADAM7;

      else if (argc >= 1 && strcmp(*argv, "--sbitlow") == 0)
         --argc, pm.sbitlow = (png_byte)atoi(*++argv);

      else if (argc >= 1 && strcmp(*argv, "--touch") == 0)
         --argc, touch = *++argv;

      else if (argc >= 1 && strncmp(*argv, "--max", 4) == 0)
      {
         --argc;

         if (strcmp(4+*argv, "abs8") == 0)
            pm.maxabs8 = atof(*++argv);

         else if (strcmp(4+*argv, "abs16") == 0)
            pm.maxabs16 = atof(*++argv);

         else if (strcmp(4+*argv, "out8") == 0)
            pm.maxout8 = atof(*++argv);

         else if (strcmp(4+*argv, "out16") == 0)
            pm.maxout16 = atof(*++argv);

         else if (strcmp(4+*argv, "pc8") == 0)
            pm.maxpc8 = atof(*++argv);

         else if (strcmp(4+*argv, "pc16") == 0)
            pm.maxpc16 = atof(*++argv);

         else
         {
            fprintf(stderr, "pngvalid: %s: unknown 'max' option\n", *argv);
            exit(1);
         }
      }

      else
      {
         fprintf(stderr, "pngvalid: %s: unknown argument\n", *argv);
         exit(1);
      }
   }

   Try
   {
      /* Make useful base images */
      make_standard_images(&pm.this);

      /* Perform the standard and gamma tests. */
      if (!pm.this.speed)
      {
         perform_standard_test(&pm);
         perform_error_test(&pm);
      }

      perform_gamma_test(&pm, pm.this.speed != 0, summary && !pm.this.speed);
   }

   Catch(fault)
   {
      fprintf(stderr, "pngvalid: test aborted (probably failed in cleanup)\n");
      if (!pm.this.verbose)
      {
         if (pm.this.error[0] != 0)
            fprintf(stderr, "pngvalid: first error: %s\n", pm.this.error);
         fprintf(stderr, "pngvalid: run with -v to see what happened\n");
      }
      exit(1);
   }

   if (summary && !pm.this.speed)
   {
      printf("Results using %s point arithmetic %s\n",
#if defined(PNG_FLOATING_ARITHMETIC_SUPPORTED) || PNG_LIBPNG_VER < 10500
         "floating",
#else
         "fixed",
#endif
         (pm.this.nerrors || (pm.this.treat_warnings_as_errors &&
            pm.this.nwarnings)) ? "(errors)" : (pm.this.nwarnings ?
               "(warnings)" : "(no errors or warnings)")
      );
      printf("Allocated memory statistics (in bytes):\n"
         "\tread  %u maximum single, %u peak, %u total\n"
         "\twrite %u maximum single, %u peak, %u total\n",
         pm.this.read_memory_pool.max_max, pm.this.read_memory_pool.max_limit,
         pm.this.read_memory_pool.max_total, pm.this.write_memory_pool.max_max,
         pm.this.write_memory_pool.max_limit,
         pm.this.write_memory_pool.max_total);
   }

   /* Do this here to provoke memory corruption errors in memory not directly
    * allocated by libpng - not a complete test, but better than nothing.
    */
   store_delete(&pm.this);

   /* Error exit if there are any errors, and maybe if there are any
    * warnings.
    */
   if (pm.this.nerrors || (pm.this.treat_warnings_as_errors &&
       pm.this.nwarnings))
   {
      if (!pm.this.verbose)
         fprintf(stderr, "pngvalid: %s\n", pm.this.error);

      fprintf(stderr, "pngvalid: %d errors, %d warnings\n", pm.this.nerrors,
          pm.this.nwarnings);

      exit(1);
   }

   /* Success case. */
   if (touch != NULL)
   {
      FILE *fsuccess = fopen(touch, "wt");

      if (fsuccess != NULL)
      {
         int error = 0;
         fprintf(fsuccess, "PNG validation succeeded\n");
         fflush(fsuccess);
         error = ferror(fsuccess);

         if (fclose(fsuccess) || error)
         {
            fprintf(stderr, "%s: write failed\n", touch);
            exit(1);
         }
      }
   }

   return 0;
}