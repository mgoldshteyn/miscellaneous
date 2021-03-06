////////////////////////////////////////////////////////////////////////////////
// compile with one of the following:
// ----------------------------------
// gcc -g -Wall -rdynamic ./simpleMemoryLibrary.c -ldl
// gcc -g -Wall -rdynamic ./simpleMemoryLibrary.c -ldl -pthread
//
// #defines for control (use -D{define}=1 to enable)
//   -DSML_PRINTF=1 : enable printf's output
//   -DTRACE=1      : enable stack trace on each allocate/free
//
// Both are disabled by default - if you never enable trace, you can omit
// -rdynamic from the compile line.
//
// This library over-rides malloc(3), realloc(3), calloc(3), and the free(3)
// functions in order to detect memory leaks, and memory over-runs.  All
// allocations and frees are reported to stdout.  Any (detected) over-run
// (or under-run) causes an abort(3).
//
// This supports pthread - if you aren't using pthreads, just disable the
// #include of pthread.h to compile out mutexes and to remove PID tracking
//
// This should be light enough to leave in a final build.
//
// Additional functions available:
//   void mem_show_allocations (FILE *fp) - shows what's currently allocated
//   int mem_get_alloc_count (void) - get the # of allocations
//   size_t mem_get_usage (void) - amount of memory allocated by the callers
//   size_t mem_get_real_usage (void) - mem_get_usage() + all over-head
//   void mem_check_integrity (void) - check all the boundaries
//   void mem_ignore_current_allocations (void) - removes currently
//      allocated blocks out of the linked list for tracking
//
// Glibc does internal allocations which it never frees, so you may see
// some outstanding allocations when your code exits.  You can suppress
// these with mem_ignore_current_allocations which just removes any allocated
// blocks from the linked list of blocks.
//
// !!NOTE!!
//
// If when using this little library you immediately get an abort() on startup
// it's most likely a failure in internalStaticAlloc (size_t size).  Basically
// calls can be made before we can use the internal allocators within glibc.
// You can fix this by changing the size of ullStatic[]
////////////////////////////////////////////////////////////////////////////////
// This code is based off from this presentation:
//    https://www.slideshare.net/tetsu.koba/tips-of-malloc-free
////////////////////////////////////////////////////////////////////////////////
// You might want to install these man pages for pthreads.  This is a note for
// myself in the future - I had a devil of a time finding these
//
//   sudo apt-get install manpages-posix manpages-posix-dev
//   sudo apt-get install glibc-doc
////////////////////////////////////////////////////////////////////////////////

#include <stdio.h>
#include <stdlib.h>
#if (defined TRACE && TRACE==1)
#include <execinfo.h>
#endif //(defined TRACE && TRACE==1)
#include <strings.h>
#include <errno.h>
#include <string.h>
#include <sys/queue.h>
#include <pthread.h> // if this is commented out, pthread support is removed

#ifndef __USE_GNU
#define __USE_GNU 1
#endif
#include <dlfcn.h>

#include "simpleMemoryLibrary.h"

#define ASSERT(v,...)                           \
do                                              \
{                                               \
  unsigned int e = (unsigned int)errno;         \
  if (!(v))                                     \
  {                                             \
    printf ("\n\nASSERT failure:\n");           \
    printf ("File: %s\n", __FILE__);            \
    printf ("Line: %d\n", __LINE__);            \
    printf ("Fun : %s\n", __FUNCTION__);        \
    printf ("Note: ");                          \
    printf (__VA_ARGS__); printf ("\n");        \
    printf ("failed expression: (%s)\n\n", #v); \
    printf ("Errno (may not be relevant):\n");  \
    printf ("  Number: 0x%08x\n", e);           \
    printf ("  String: %s\n", strerror(e));     \
    abort ();                                   \
  }                                             \
}                                               \
while (0)


#define MEM_HEADER_GUARD_LEN (2)
#define MEM_CAP_GUARD_LEN    (2)
#define GUARD_BAND_TOP       (0xDEADBEEFCAFEF00DULL)
#define GUARD_BAND_BOTTOM    (0x0CACAFECEBADC0DEULL)

LIST_HEAD (listHead, memoryHeader) gp_listHead;
struct memoryHeader
{
  char *szAllocator;
  size_t size;
#ifdef _PTHREAD_H
  pthread_t threadId;
#endif //_PTHREAD_H
  LIST_ENTRY (memoryHeader) doubleLL;
  unsigned long long ullFixedValues[MEM_HEADER_GUARD_LEN];
};

struct memoryCap
{
  unsigned long long ullFixedValues[MEM_CAP_GUARD_LEN];
};

#ifdef _PTHREAD_H
pthread_mutex_t g_mutex;
#endif //_PTHREAD_H

static __thread int gi_hookDisabled=0;
static int gi_allocCount=0;
static void * (*gp_orgMalloc)  (size_t size)              = NULL;
static void   (*gp_orgFree)    (void *ptr)                = NULL;
static void * (*gp_orgCalloc)  (size_t nmeb, size_t size) = NULL;
static void * (*gp_orgRealloc) (void *ptr, size_t size)   = NULL;

void static init (void) __attribute__((constructor)); // initialize this library
void static end  (void) __attribute__((destructor));  // check for any outstanding allocs

#if (defined SML_PRINTF && SML_PRINTF==1)
#undef SML_PRINTF
#define SML_PRINTF(...) printf (__VA_ARGS__)
#else
#define SML_PRINTF(...)
#endif //(defined SML_PRINTF && SML_PRINTF==1)

#ifdef _PTHREAD_H

// wrappers for mutex so I don't have to bother with checking error conditions.
#define MUTEX_INIT(mp)                  \
do                                      \
{                                       \
  if (0 && gi_hookDisabled == 0)        \
  {                                     \
    SML_PRINTF ("MUTEX_INIT\n");        \
  }                                     \
  if (pthread_mutex_init(mp,NULL) != 0) \
  {                                     \
    perror("pthread_mutex_init");       \
    abort ();                           \
  }                                     \
} while (0)

#define MUTEX_LOCK(mp)                  \
do                                      \
{                                       \
  if (0 && gi_hookDisabled == 0)        \
  {                                     \
    SML_PRINTF ("MUTEX_LOCK\n");        \
  }                                     \
  if (pthread_mutex_lock(mp) != 0)      \
  {                                     \
    perror("pthread_mutex_lock");       \
    abort ();                           \
  }                                     \
} while (0)

#define MUTEX_UNLOCK(mp)                \
do                                      \
{                                       \
  if (0 && gi_hookDisabled == 0)        \
  {                                     \
    SML_PRINTF ("MUTEX_UNLOCK\n");      \
  }                                     \
  if (pthread_mutex_unlock(mp) != 0)    \
  {                                     \
    perror("pthread_mutex_unlock");     \
    abort ();                           \
  }                                     \
} while (0)

#define MUTEX_DESTROY(mp)               \
do                                      \
{                                       \
  if (0 && gi_hookDisabled == 0)        \
  {                                     \
    SML_PRINTF ("MUTEX_DESTROY\n");     \
  }                                     \
  if (pthread_mutex_destroy(mp) != 0)   \
  {                                     \
    perror("pthread_mutex_destroy");    \
    abort ();                           \
  }                                     \
} while (0)

#else //! _PTHREAD_H
#define MUTEX_INIT(mp)
#define MUTEX_LOCK(mp)
#define MUTEX_UNLOCK(mp)
#define MUTEX_DESTROY(mp)
#endif //_PTHREAD_H

static void init (void);
static void end (void);
static char *trace (int iLen, unsigned ucGetPtr);
static struct memoryHeader *verifyIntegrity (void *vPtr);
static void *internalRealloc (void *vPtr, size_t size, size_t nmemb, unsigned char type);
static void internalFree (void *vPtr, int iLen);

static void init (void)
{
  // only realloc and free are actually used, but I keep pointers to all
  gp_orgMalloc  = (void* (*)(long unsigned int)) dlsym (RTLD_NEXT, "malloc");
  gp_orgFree    = (void  (*)(void*)) dlsym (RTLD_NEXT, "free");
  gp_orgCalloc  = (void* (*)(long unsigned int, long unsigned int)) dlsym (RTLD_NEXT, "calloc");
  gp_orgRealloc = (void* (*)(void*, long unsigned int)) dlsym (RTLD_NEXT, "realloc");
  MUTEX_INIT (&g_mutex);

  LIST_INIT (&gp_listHead);
  malloc (0);
}

int mem_get_alloc_count (void)
{
  return gi_allocCount;
}

size_t mem_get_usage (void)
{
  struct memoryHeader *ml;
  size_t size=0;

  MUTEX_LOCK (&g_mutex);
  for (ml = gp_listHead.lh_first ;
       ml != NULL ;
       ml = ml->doubleLL.le_next)
  {
    verifyIntegrity (ml+1);
    if (ml->szAllocator != NULL)
    {
      size += ml->size;
    }
  }
  MUTEX_UNLOCK (&g_mutex);

  return size;
}

size_t mem_get_real_usage (void)
{
  struct memoryHeader *ml;
  size_t size=0;

  MUTEX_LOCK (&g_mutex);
  for (ml = gp_listHead.lh_first ;
       ml != NULL ;
       ml = ml->doubleLL.le_next)
  {
    verifyIntegrity (ml+1);
    size += ml->size + sizeof(struct memoryHeader) + sizeof(struct memoryCap);
  }
  MUTEX_UNLOCK (&g_mutex);

  return size;
}

void mem_check_integrity (void)
{
  struct memoryHeader *ml;

  MUTEX_LOCK (&g_mutex);
  for (ml = gp_listHead.lh_first ;
       ml != NULL ;
       ml = ml->doubleLL.le_next)
  {
    verifyIntegrity (ml+1);
  }
  MUTEX_UNLOCK (&g_mutex);
}

void mem_ignore_current_allocations (void)
{
  struct memoryHeader *ml;

  MUTEX_LOCK (&g_mutex);
  for (ml = gp_listHead.lh_first ;
       ml != NULL ;
       ml = ml->doubleLL.le_next)
  {
    LIST_REMOVE (ml, doubleLL);
    ml->doubleLL.le_prev = NULL;
  }
  gi_allocCount = 0;
  MUTEX_UNLOCK (&g_mutex);
}

void mem_show_allocations (FILE *fp)
{
  struct memoryHeader *ml;
  int iCount=0;

  MUTEX_LOCK (&g_mutex);
  for (ml = gp_listHead.lh_first ;
       ml != NULL ;
       ml = ml->doubleLL.le_next)
  {
    verifyIntegrity (ml+1);
    if (ml->szAllocator != NULL && ml->size != 0)
    {
      if (iCount++==0)
      {
        // write a header
        fprintf (fp, "\n");
        int iLen = fprintf (fp, "%d block%s remains allocated\n",
                            gi_allocCount, gi_allocCount != 1 ? "s":"");
        while (--iLen > 0)
        {
          fprintf (fp, "-");
        }
        fprintf (fp, "\n");
      }
      fprintf (fp, "  Address %p size of %zu, allocated by \"%s\"\n",
               ml->ullFixedValues+MEM_HEADER_GUARD_LEN, ml->size, ml->szAllocator);
    }
  }

  if (iCount != 0)
  {
    fprintf (fp, "\n");
  }
  else
  {
    fprintf (fp, "No memory allocations currently\n");
  }
  MUTEX_UNLOCK (&g_mutex);
}

static void end (void)
{
  mem_show_allocations (stderr);
  mem_check_integrity ();
  MUTEX_DESTROY (&g_mutex);
}

#if (defined _EXECINFO_H && _EXECINFO_H == 1)
static size_t getFunction (char *szDst, size_t sOffset, char *szSrc, size_t sMax)
{
  size_t sSrc;
  size_t sDst=sOffset;
  int iMode=0;

  for (sSrc = 0 ; szSrc[sSrc] != '\0' ; sSrc++)
  {
    if (sDst == sMax)
    {
      szDst[sMax-1] = '\0';

      break;
    }

    switch (szSrc[sSrc])
    {
    case '(' :
      iMode = 1;
      break;
    case '\0' :
    case ')' :
      iMode = 2;
      if (sDst < sMax)
      {
        szDst[sDst++] = '<';
      }
      if (sDst < sMax)
      {
        szDst[sDst++] = '<';
      }
      if (sDst < sMax)
      {
        szDst[sDst++] = '-';
      }
      break;
    default:
      if (iMode == 1 && sDst < sMax)
      {
        szDst[sDst++] = szSrc[sSrc];
      }
    }
  }

  return sDst;
}

static char *trace (int iLen, unsigned ucGetPtr)
{
  char *szPtr=NULL;
  int nptrs;
  char **strings;
  void *buffer[200];

  nptrs = backtrace(buffer, sizeof(buffer)/sizeof(buffer[0]));

  strings = backtrace_symbols(buffer, nptrs);
  if (strings == NULL)
  {
    perror("backtrace_symbols");
    exit(EXIT_FAILURE);
  }

  if (ucGetPtr == 0)
  {
    int j;
    for (j = 0; j < nptrs; j++)
    {
      SML_PRINTF("TRACE> %s\n", strings[j]);
    }
  }
  else
  {
    const size_t maxSize = 1024;
    int i;
    size_t sPos = 0;

    szPtr = (char *)realloc (szPtr, maxSize);
    ASSERT (szPtr != NULL, "szPtr is NULL");
    for (i = iLen-1; i < nptrs; i++)
    {
      sPos = getFunction (szPtr, sPos, strings[i], maxSize);
    }

    if (sPos != 0)
    {
      szPtr[sPos-3] = '\0';
      szPtr = (char *)realloc (szPtr, sPos-2);
    }
    else
    {
      // this should never happen..
      free (szPtr);
      szPtr = NULL;
    }
  }

  free(strings);

  return szPtr;
}
#else
static char *trace (int iLen, unsigned ucGetPtr)
{
  (void)iLen;
  (void)ucGetPtr;
  static char szEmpty[] = "traceDisabled";

  return szEmpty;
}
#endif //(defined _EXECINFO_H && _EXECINFO_H == 1)

static struct memoryHeader *verifyIntegrity (void *vPtr)
{
  struct memoryHeader *mHead;
  struct memoryCap    *mCap;
  unsigned char *ucPtr;
  size_t s;
  size_t size;
  int i;

  // adjust pointer to the actual start of allocation
  mHead = ((struct memoryHeader *)(vPtr))-1;

  size = mHead->size;

  for (i = 0 ; i < MEM_HEADER_GUARD_LEN ; i++)
  {
    ASSERT (mHead->ullFixedValues[i] == GUARD_BAND_TOP,
            "Top guard band %d corrupt expected 0x%016llX got 0x016%llX - this is BEFORE allocated memory\n",
            i, GUARD_BAND_TOP, mHead->ullFixedValues[i]);
  }

  ucPtr = ((unsigned char *) vPtr);
  for (s = size ;
       ((unsigned long long) (ucPtr + s)) % (sizeof (unsigned long long));
       s++)
  {
    ASSERT (ucPtr[s] == (unsigned char) (((unsigned long long) (ucPtr+s)) & 0xFF),
            "end of alloc memory over-written %zu bytes beyond end", 1+s-size);
  }
  mCap = (struct memoryCap *)(ucPtr + s);

  for (i = 0 ; i < MEM_CAP_GUARD_LEN ; i++)
  {
    ASSERT (mCap->ullFixedValues[i] == GUARD_BAND_BOTTOM,
            "Bottom guard band %d corrupt expected 0x%016llX got 0x%016llX - this is AFTER allocated memory\n",
            i, GUARD_BAND_BOTTOM, mCap->ullFixedValues[i]);
  }

  return mHead;
}

#define REALLOC 0
#define MALLOC  1
#define CALLOC  2
static void *internalRealloc (void *vPtr, size_t size, size_t nmemb, unsigned char type)
{
  struct memoryHeader *mHead = NULL;
  struct memoryCap    *mCap = NULL;
  unsigned char *ucPtr;
  size_t adjSize;
  size_t s;
  char *szCaller = NULL;
  char *szAllocator = NULL;
  int i;

  // NOTE: In this implementation, a size of 0 can be allocated
  //       This is POSIX compliant.  If the memory that is allocated
  //       is modified, it will be detected on free.

  adjSize = size*nmemb;
  adjSize += sizeof (struct memoryHeader) + sizeof (struct memoryCap);

  // align on a unsigned long long
  if (adjSize % sizeof(unsigned long long))
  {
    adjSize += sizeof(unsigned long long) - (adjSize % sizeof(unsigned long long));
  }

  // adjust pointer to the actual start of allocation
  if (vPtr != NULL)
  {
    // we have to remove this from the linked list because we are reallocating
    // the memory - which may move it.  Verify the integrity of the memory as
    // well.
    mHead = verifyIntegrity (vPtr);
    szAllocator = mHead->szAllocator;
    MUTEX_LOCK (&g_mutex);
    if (mHead->doubleLL.le_prev != NULL)
    {
      LIST_REMOVE (mHead, doubleLL);
      mHead->doubleLL.le_prev = NULL;
    }
    MUTEX_UNLOCK (&g_mutex);
  }
  mHead = (struct memoryHeader *)gp_orgRealloc (mHead, adjSize);
  if (mHead == NULL)
  {
    return NULL;
  }

  if (!gi_hookDisabled)
  {
    gi_hookDisabled = 1;

    szCaller = trace (4, 1);
    switch (type)
    {
    case REALLOC:
      // alloc count doesn't change - even if 0 size is being actually
      // allocated - unless vPtr == NULL
      if (vPtr == NULL)
      {
        MUTEX_LOCK (&g_mutex);
        gi_allocCount++;
        MUTEX_UNLOCK (&g_mutex);
      }
      SML_PRINTF ("realloc (%p, %zu) = %p, allocated by %s (org: %s) %d\n",
                  vPtr, size, &mHead->ullFixedValues[MEM_HEADER_GUARD_LEN], szCaller, szAllocator, gi_allocCount);
#if (defined _EXECINFO_H && _EXECINFO_H == 1)
      if (szAllocator != NULL)
      {
        free (szAllocator);
      }
#else
      (void) szAllocator;
#endif //(defined _EXECINFO_H && _EXECINFO_H == 1)
      break;

    case MALLOC:
      MUTEX_LOCK (&g_mutex);
      gi_allocCount++;
      MUTEX_UNLOCK (&g_mutex);
      SML_PRINTF ("malloc (%zu) = %p, allocated by %s, %d\n",
                  size, &mHead->ullFixedValues[MEM_HEADER_GUARD_LEN], szCaller, gi_allocCount);
      break;

    case CALLOC:
      MUTEX_LOCK (&g_mutex);
      gi_allocCount++;
      MUTEX_UNLOCK (&g_mutex);
      SML_PRINTF ("calloc (%zu, %zu) = %p, allocated by %s, %d\n",
                  nmemb, size, &mHead->ullFixedValues[MEM_HEADER_GUARD_LEN], szCaller, gi_allocCount);
      break;
    }
    gi_hookDisabled = 0;
  }

  // save the allocator and size, fill up any unused bytes at the end of
  // the allocation with essentially address == data, and place guard bands
  // on the memory at the bottom and top of memory
  mHead->szAllocator = szCaller;
  mHead->size = size*nmemb;
#ifdef _PTHREAD_H
  mHead->threadId = pthread_self ();
#endif //_PTHREAD_H
  MUTEX_LOCK (&g_mutex);
  mHead->doubleLL.le_next = NULL;
  mHead->doubleLL.le_prev = NULL;
  LIST_INSERT_HEAD(&gp_listHead, mHead, doubleLL);
  MUTEX_UNLOCK (&g_mutex);
  for (i = 0 ; i < MEM_HEADER_GUARD_LEN ; i++)
  {
    mHead->ullFixedValues[i] = GUARD_BAND_TOP;
  }

  // point to usable memory
  ucPtr = ((unsigned char *)(mHead+1));
  for (s = mHead->size ;
       ((unsigned long long) (ucPtr + s)) % (sizeof (unsigned long long));
       s++)
  {
    ucPtr[s] = (unsigned char) (((unsigned long long) (ucPtr+s)) & 0xFF);
  }

  // fill up the cap
  mCap = (struct memoryCap *)(ucPtr + s);
  for (i = 0 ; i < MEM_CAP_GUARD_LEN ; i++)
  {
    mCap->ullFixedValues[i] = GUARD_BAND_BOTTOM;
  }

  vPtr = ucPtr;
  return vPtr;
}

static void *internalStaticAlloc (size_t size)
{
  // glibc and C++ can make use of malloc and calloc before the init can be
  // called.  When this happens we have to pass back some usable memory.
  // This memory is not freed() on exit - at least currently - if it is
  // later, you'll have to modify the free and possibly realloc functions
  // as well to detect this is statically allocated and not really part
  // of the heap.

  // C++ allocates quite a large block of memory on startup..
  static __thread unsigned long long ullStatic[0x3000];
  static __thread size_t sIndex = 0;
  void *vPtr;

  vPtr = &ullStatic[sIndex];

  // find next spot to "allocate" until init() can be called
  // in case it happens multiple times
  sIndex += (size + (sizeof(ullStatic[0])-1)) / sizeof (ullStatic[0]);

  // if we hit abort(), we have allocated more memory on startup than
  // we made available, make ullStatic larger

  // I can't use my regular ASSERT here, because it has a printf, and
  // that needs allocated memory too, just abort
  //
  //ASSERT (sIndex < (sizeof (ullStatic) / sizeof (ullStatic[0])),
  //        "Out of memory, index = %zu max index %zu", sIndex,
  //        (sizeof (ullStatic) / sizeof (ullStatic[0])));
  if (sIndex >= (sizeof (ullStatic) / sizeof (ullStatic[0])))
  {
    // You ran out of memory before dlsym(3) could be called,
    // increase the size of ullStatic.  You can figure out how much
    // you need based off from the current value of sIndex, at least
    // for this ONE allocation.
    
    abort ();
  }

  return vPtr;
}

static void internalFree (void *vPtr, int iLen)
{
  struct memoryHeader *mHead;
  char *szCaller = NULL;
  char *szAllocator = NULL;

  // verify no over-runs in data
  mHead = verifyIntegrity (vPtr);

  szAllocator = mHead->szAllocator;

  MUTEX_LOCK (&g_mutex);
  if (mHead->doubleLL.le_prev != NULL)
  {
    LIST_REMOVE (mHead, doubleLL);
    mHead->doubleLL.le_prev = NULL;
  }
  MUTEX_UNLOCK (&g_mutex);
  gp_orgFree (mHead);

  if (!gi_hookDisabled)
  {
    gi_hookDisabled = 1;
    szCaller = trace (iLen, 1);
    if (szAllocator != NULL)
    {
      SML_PRINTF ("free (%p) (allocated by \"%s\" freed by \"%s\"), %d\n",
                  vPtr, szAllocator, szCaller, gi_allocCount);
    }
    MUTEX_LOCK (&g_mutex);
    gi_allocCount--;
    MUTEX_UNLOCK (&g_mutex);

#if (defined _EXECINFO_H && _EXECINFO_H == 1)
    if (szCaller != NULL)
    {
      free (szCaller);
    }
    if (szAllocator != NULL)
    {
      free (szAllocator);
    }
#else
    (void) szCaller;
#endif //(defined _EXECINFO_H && _EXECINFO_H == 1)
    gi_hookDisabled = 0;
  }
}

void *malloc (size_t size)
{
  void *vPtr;

  if (gp_orgMalloc == NULL)
  {
    // C++ allocates memory before init() can be called in this module
    vPtr = internalStaticAlloc (size);
  }
  else
  {
    vPtr = internalRealloc (NULL, size, 1, MALLOC);
  }

  return vPtr;
}

void *realloc(void *vPtr, size_t size)
{
  // if we hit this, some library that was called before this
  // library's init() was called.  You'll have to go through a
  // debugger to see what request is being made and then write
  // the appropriate code.
  ASSERT (gp_orgRealloc != NULL,
          "realloc called before gp_orgRealloc set");

  // you can free memory with realloc, if you pass 0 size, with a
  // non NULL pointer however, I can see in the realloc(3) implementation
  // under linux, this will return a pointer which you can free, so
  // I alloc an allocation of 0 size, which returns a block of memory
  // that has no space to write to.
  return internalRealloc (vPtr, size, 1, REALLOC);
}

void *calloc(size_t nmemb, size_t size)
{
  void *vPtr;

  if (gp_orgCalloc == NULL)
  {
    // When compiling against -pthread, calloc() is called by dlsym(3)
    // but we need dlsym(3) to get the pointers to glib's functions
    // like calloc() - we will return some statically allocated memory.
    vPtr = internalStaticAlloc (nmemb * size);
  }
  else
  {
    vPtr = internalRealloc (NULL, nmemb, size, CALLOC);
  }

  memset (vPtr, 0, nmemb*size);
  return vPtr;
}

void free(void *vPtr)
{
  // a pointer value of NULL is legal under POSIX, oddly
  if (vPtr != NULL)
  {
    internalFree (vPtr, 4);
  }
}
