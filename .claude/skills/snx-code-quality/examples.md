# snx-code-quality: the rules with their wrong and right forms

Every C block here is strict ANSI C (`-std=c90 -pedantic-errors`), and each wrong/right pair gives the same
results under ASan and UBSan except where the right form is meant to differ. Read the rule in doubt.

## 1. No state another value already says

**Do:** let the value carry its own presence. A pointer is absent when it is NULL.

**Why:** two fields saying one thing drift apart. Once `is_present` and `p` disagree, every reader has
to guess which one is true.

<!-- snippet: flag-wrong -->
```c
typedef struct {
    void* p;
    int is_present;
} Opt;
int opt_present(const Opt* o) { return o->is_present; }
```

<!-- snippet: flag-right -->
```c
typedef struct {
    void* p; /* NULL: not present */
} Opt;
int opt_present(const Opt* o) { return o->p != NULL; }
```

**Does not apply when:**
- NULL is a value of its own ("present and empty").
- Every value of the type is valid (a `u32` offset where 0 is a real offset). Then reserve zero first
  (leave slot or byte 0 unused, so 0 still means none, as in rule 2); keep a flag only when nothing can
  be reserved.
- The flag says something else: `has_keys` in `extract.c` says the keys file exists, while `keys`
  always holds a path.

## 2. Indices, not pointer chains

**Do:** keep the objects in one array and link them by index (`u32`), with 0 as none: slot 0 of the
array is never a real object. Not `UINT32_MAX` or -1: `if (!n)` then reads like a null pointer check,
a compare with zero is the cheapest there is, and no index needs an out-of-range case. To sort a list,
sort an array of (key, index) pairs with `qsort` instead of merge-sorting `next` pointers.

**Why:** walking an array touches memory in order, while a chain of pointers jumps across the heap. An
index stays valid when the array is reallocated and when the array is written out (rule 5). A pointer
does neither.

<!-- snippet: indices-wrong -->
```c
typedef struct FsNode {
    struct FsNode* parent;
    int attached;
} FsNode;
typedef struct {
    FsNode* root;
} FsTree;
int belongs_to(const FsTree* tree, const FsNode* n) {
    if (!n || !n->attached) return 0;
    while (n->parent) {
        n = n->parent;
        if (!n->attached) return 0;
    }
    return n == tree->root;
}
```

<!-- snippet: indices-right -->
```c
typedef struct {
    u32 parent; /* 0 at the top */
    int attached;
} FsNode;
typedef struct {
    FsNode* nodes; /* every node of the tree; nodes[0] is none, so index 0 means no node */
    u32 root;
} FsTree;
int belongs_to(const FsTree* tree, u32 n) {
    if (!n || !tree->nodes[n].attached) return 0;
    while (tree->nodes[n].parent) {
        n = tree->nodes[n].parent;
        if (!tree->nodes[n].attached) return 0;
    }
    return n == tree->root;
}
```

**Does not apply when:**
- The code holding the index cannot reach the array, or the array dies first. An index means nothing
  without its array, so pass the owner along.
- Objects come and go in the middle of the array. Removal leaves a hole (keep a free list) or
  renumbers everything after it (fix every index that pointed there).
- Objects of different owners link to each other, like a thread waiting on several kernel objects.
  Use a pointer there, or an index into one table they all share.

## 3. One allocation per owner

**Do:** an object's arrays come from one allocation carved by offsets, and are freed with one `free`.
When the sizes are fixed, a struct holding the arrays is the simplest block. When they are known only
at run time, put each array at a multiple of its type's size: a size is always a multiple of the
alignment, and C90 has no `alignof`.

**Why:** one block keeps an owner's data together and leaves less to fragment. It also turns a
cleanup path with one `free` per array into a single `free`.

<!-- snippet: block-wrong -->
```c
typedef struct {
    Voice* voice;
    Mix* mix;
} Mixer;
int mixer_init(Mixer* m, size_t voices, size_t mixes) {
    m->voice = (Voice*)calloc(voices, sizeof *m->voice);
    m->mix = (Mix*)calloc(mixes, sizeof *m->mix);
    return m->voice && m->mix; /* a failure leaks the one that worked */
}
void mixer_free(Mixer* m) {
    free(m->voice);
    free(m->mix);
}
```

<!-- snippet: block-right -->
```c
typedef struct {
    Voice* voice; /* the block's start: free(voice) frees both */
    Mix* mix;     /* after the voices, at a multiple of sizeof(Mix) */
} Mixer;
int mixer_init(Mixer* m, size_t voices, size_t mixes) {
    const size_t most = (size_t)-1; /* SIZE_MAX is C99 */
    size_t mix_at, total;
    char* block;
    if (voices > (most - sizeof(Mix)) / sizeof(Voice) || mixes > most / sizeof(Mix)) return 0;
    mix_at = (voices * sizeof(Voice) + sizeof(Mix) - 1) / sizeof(Mix) * sizeof(Mix);
    if (mixes * sizeof(Mix) > most - mix_at) return 0;
    total = mix_at + mixes * sizeof(Mix);
    block = (char*)calloc(1, total ? total : 1);
    if (!block) return 0;
    m->voice = (Voice*)block;
    m->mix = (Mix*)(block + mix_at);
    return 1;
}
void mixer_free(Mixer* m) { free(m->voice); }
```

`calloc(n, size)` checked each product for overflow. Products and sums of your own do not, hence the checks
against `(size_t)-1`, which matter when a size comes from the guest.

**Does not apply when:**
- One of the arrays grows on its own. A `realloc` moves the whole block, so give that array a block of
  its own.
- The parts live for different lengths of time.
- A part is large and rarely used: allocate it when it is first needed.
- What you are building starts to look like a general allocator (free lists across owners, size
  classes). The RFC asks for chunking, not for a new malloc.

## 4. Atomics on cache lines of their own

**Do:** when two atomics (or a lock and the data next to it) are written by different threads, give each
its own cache line with a `char` array between them. Atomics are plain integers reached through the
`__atomic_*` builtins, as `src/` does, not C11's `_Atomic`. Defining two next to each other at file scope
puts them together too: keep them in one padded struct instead.

**Why:** a core writes a whole 64-byte line. Two threads writing two variables in the same line take
that line from each other on every write, as slowly as if they wrote one shared variable. This is
false sharing.

<!-- snippet: sharing-wrong -->
```c
typedef struct {
    u32 submitted; /* the CPU thread's, through __atomic_* */
    u32 completed; /* the GPU thread's */
} Queue;
```

<!-- snippet: sharing-right -->
```c
#define SNX_CACHE_LINE 64 /* x86-64 */
typedef struct {
    u32 submitted; /* the CPU thread's, through __atomic_* */
    char apart[SNX_CACHE_LINE - sizeof(u32)];
    u32 completed; /* the GPU thread's */
} Queue;
/* C90's static assert: an array of negative size if they would share a line */
typedef char snx_queue_apart[offsetof(Queue, completed) - offsetof(Queue, submitted) >= SNX_CACHE_LINE ? 1 : -1];
```

**Does not apply when:**
- One thread writes both, or they are flags read often and written rarely. Padding then buys nothing.
- There are many of them. Each padded atomic costs a full line, and in an array of per-object atomics
  that adds up.

The padding keeps the two apart from each other, not from a neighbouring allocation: `malloc` aligns to
16, not 64. Pad before the first one too when that matters.

## 5. No stored pointers in data

**Do:** a structure that is data (tables, trees, anything that could be saved or shared) refers to its
parts by index or by offset from its own start, not by address. Native byte order and widths are fine.
The point is that the block means the same wherever it is copied.

**Why:** an address is only valid in this process and at this moment. Data with offsets can be copied,
written out, read back or mapped elsewhere without fixing it up.

<!-- snippet: serial-wrong -->
```c
typedef struct Node {
    const char* name;    /* a string somewhere on the heap */
    struct Node* parent; /* an address: wrong in a copy, meaningless once written out */
} Node;
```

<!-- snippet: serial-right -->
```c
typedef struct {
    u32 name;   /* offset of its NUL-terminated name in the names that follow the nodes */
    u32 parent; /* index in the nodes, 0 at the top: nodes[0] is none */
} Node;
typedef struct {
    u32 count; /* then Node[count], then the names: one block, copied or written out as is */
} NodeTable;
const Node* table_node(const NodeTable* t, u32 i) { return (const Node*)(t + 1) + i; }
const char* table_name(const NodeTable* t, u32 i) {
    return (const char*)((const Node*)(t + 1) + t->count) + table_node(t, i)->name;
}
```

**Does not apply when:**
- The member is a handle to something outside the data: `FILE*`, `SDL_Window*`, a Vulkan object, a
  function pointer. Keep those in a runtime struct beside the data, not inside it.
- The structure is never copied, written out or shared, and a pointer is plainly simpler. The RFC's
  words are "unless absolutely necessary", so say why in a comment.
- The access is hot enough that an add per access shows up in a measurement. Measure first.

## 6. One global per file

**Do:** keep a file's mutable state in one struct and define one global of it, `g_<file>` (static unless
another file needs it). Function-local `static`s are globals too and go in it. What another file reads is
declared once in the header next to the file: the struct's type with the global, or a function that
answers it. Atomics that different threads write still get a cache line each (rule 4), padded inside it.

In `src/nx/hle/` the struct is a member of `g_hle` (`hle/hle.h`) instead of a global of the file: its type
and its initializer macro (`HLE_IPC_INIT`) are in the header beside the file, `hle.c` defines `g_hle` with
them, and the code reads `g_hle.ipc.lk`. The objects the file makes and frees are pools in that struct, not
`malloc`s: a static array with a bound, taken with `POOL_TAKE` under the lock that guards it and made non-zero
before the lock is let go, freed with `POOL_PUT`.

**Why:** loose globals scatter one subsystem's state over a file, each with its own name, initialization and
lock, so nothing shows what belongs together or what a lock guards. One struct is one place to read, reset in
a test, dump in a crash report or move into a context later.

<!-- snippet: globals-wrong -->
```c
static Session g_sessions[SESSION_MAX];
static u32 g_session_next;
static PMutex g_pool_lk = PMUTEX_INIT;
u32 g_ring_n; /* read by debug.c through an extern of its own */
static int warn_once(u32 cmd) {
    static u32 seen[512]; /* a global no one sees */
    ...
}
```

<!-- snippet: globals-right -->
```c
static struct {
    PMutex lk; /* guards sessions and next */
    Session sessions[SESSION_MAX];
    u32 next;
    u32 seen[512]; /* warn_once's */
    char apart[SNX_CACHE_LINE];
    u32 ring_n; /* every thread's, through __atomic_* */
} g_ipc = {PMUTEX_INIT};
u32 ipc_ring_count(void) { return __atomic_load_n(&g_ipc.ring_n, __ATOMIC_RELAXED); } /* debug.c's */
```

**Does not apply when:**
- The object is `const`: a table is data, not state.
- The variable is `_Thread_local`: each thread has its own, so it can't live in a shared struct.
- A third-party file (`.cache/cpm`), which is never edited.
