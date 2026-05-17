// Tiny regex engine — Russ Cox's "re1" Thompson-NFA design,
// trimmed to C99 + no stdlib. Compiles to wasm via wasi-sdk
// with NO WASI imports (no allocator, no I/O).
//
// Supported syntax:
//   .              any single char
//   ^ $            anchors
//   *  +  ?        repetition
//   \d \w \s       digit / word / whitespace classes
//   [abc] [^abc]   character classes
//   |              alternation
//   ( )            grouping (no captures)
//
// This is the subset RE2 calls "POSIX basic + a bit"; matches the
// "fast path" we'd ship in option E (hybrid) from PLAN.md.

#include <stdint.h>
#include <stddef.h>

typedef struct State State;
struct State {
  int32_t c;           // char to consume, or -1 for split, or -2 for match
  State* out;
  State* out1;
  int32_t mark;
};

#define MATCH_STATE_C   (-2)
#define SPLIT_STATE_C   (-1)

#define MAX_STATES 256
static State pool[MAX_STATES];
static int32_t pool_n = 0;

static State* newstate(int32_t c, State* o, State* o1) {
  if (pool_n >= MAX_STATES) return NULL;
  State* s = &pool[pool_n++];
  s->c = c; s->out = o; s->out1 = o1; s->mark = 0;
  return s;
}

// Compile a tiny regex syntax tree -> NFA. We compile in-place
// from the source string; this is a recursive-descent parser.
// Returns the start state, or NULL on parse error.

typedef struct Parser {
  const char* p;
  const char* end;
  int32_t error;
} Parser;

static State* parse_alt(Parser*);

static int char_in_class(char target, const char* cls, size_t clen, int negate) {
  int hit = 0;
  for (size_t i = 0; i < clen; ++i) {
    if (i + 2 < clen && cls[i+1] == '-') {
      if (target >= cls[i] && target <= cls[i+2]) { hit = 1; break; }
      i += 2;
    } else if (cls[i] == target) { hit = 1; break; }
  }
  return negate ? !hit : hit;
}

// To keep the NFA simple, character classes are stored as small
// "class atoms" — we encode them by interning the class body and
// matching against it in the consumer.  For this prototype we
// just bake the class into a custom state c-value: c = -10 means
// "look at extra[mark] for class".
#define CLASS_BASE (-10)
static const char* class_bodies[32];
static int32_t class_negate[32];
static size_t class_lens[32];
static int32_t class_n = 0;

static State* parse_atom(Parser* P) {
  if (P->p >= P->end) return NULL;
  char c = *P->p;
  if (c == '(') {
    P->p++;
    State* s = parse_alt(P);
    if (!s || P->p >= P->end || *P->p != ')') { P->error = 1; return NULL; }
    P->p++;
    return s;
  }
  if (c == '[') {
    P->p++;
    int negate = 0;
    if (P->p < P->end && *P->p == '^') { negate = 1; P->p++; }
    const char* start = P->p;
    while (P->p < P->end && *P->p != ']') P->p++;
    if (P->p >= P->end) { P->error = 1; return NULL; }
    if (class_n >= 32) { P->error = 1; return NULL; }
    class_bodies[class_n] = start;
    class_lens[class_n] = (size_t)(P->p - start);
    class_negate[class_n] = negate;
    State* s = newstate(CLASS_BASE - class_n, NULL, NULL);
    if (s) s->mark = class_n;
    class_n++;
    P->p++;  // skip ']'
    return s;
  }
  if (c == '\\' && P->p + 1 < P->end) {
    char e = P->p[1];
    P->p += 2;
    int32_t code = -100;  // placeholder for escape classes
    if (e == 'd') code = -1001;
    else if (e == 'w') code = -1002;
    else if (e == 's') code = -1003;
    else if (e == 'D') code = -1004;
    else if (e == 'W') code = -1005;
    else if (e == 'S') code = -1006;
    else return newstate((int32_t)(unsigned char)e, NULL, NULL);
    return newstate(code, NULL, NULL);
  }
  if (c == '.') { P->p++; return newstate(-200, NULL, NULL); }  // any
  if (c == '^') { P->p++; return newstate(-201, NULL, NULL); }  // begin
  if (c == '$') { P->p++; return newstate(-202, NULL, NULL); }  // end
  P->p++;
  return newstate((int32_t)(unsigned char)c, NULL, NULL);
}

static State* parse_piece(Parser* P) {
  State* a = parse_atom(P);
  if (!a) return NULL;
  if (P->p < P->end) {
    char r = *P->p;
    if (r == '*' || r == '+' || r == '?') {
      P->p++;
      State* loop = newstate(SPLIT_STATE_C, a, NULL);
      if (!loop) return NULL;
      a->out = (r == '?') ? NULL : loop;
      if (r == '+') return loop;  // but enter through a first
      return loop;
    }
  }
  return a;
}

static State* parse_concat(Parser* P) {
  State* head = NULL;
  State* tail = NULL;
  while (P->p < P->end && *P->p != '|' && *P->p != ')') {
    State* piece = parse_piece(P);
    if (!piece) return NULL;
    if (!head) { head = tail = piece; continue; }
    // Wire tail->out to piece's entry.
    State* t = tail;
    while (t->out) t = t->out;
    t->out = piece;
    tail = piece;
  }
  return head;
}

static State* parse_alt(Parser* P) {
  State* l = parse_concat(P);
  if (!l) return NULL;
  if (P->p < P->end && *P->p == '|') {
    P->p++;
    State* r = parse_alt(P);
    if (!r) return NULL;
    return newstate(SPLIT_STATE_C, l, r);
  }
  return l;
}

// Eval — set-of-states simulation (Thompson, O(n*m)).

static int matches_class(int32_t code, char target) {
  // Predefined escape classes.
  if (code == -1001) return target >= '0' && target <= '9';
  if (code == -1002) return (target>='a'&&target<='z')||(target>='A'&&target<='Z')||(target>='0'&&target<='9')||target=='_';
  if (code == -1003) return target==' '||target=='\t'||target=='\n'||target=='\r';
  if (code == -1004) return !((target >= '0' && target <= '9'));
  if (code == -1005) return !((target>='a'&&target<='z')||(target>='A'&&target<='Z')||(target>='0'&&target<='9')||target=='_');
  if (code == -1006) return !(target==' '||target=='\t'||target=='\n'||target=='\r');
  if (code <= CLASS_BASE) {
    int idx = CLASS_BASE - code;
    return char_in_class(target, class_bodies[idx], class_lens[idx], class_negate[idx]);
  }
  return 0;
}

static int32_t listid = 0;
static State* clist[MAX_STATES];
static State* nlist[MAX_STATES];

static void add_state(State** list, int* nl, State* s) {
  if (!s || s->mark == listid) return;
  s->mark = listid;
  if (s->c == SPLIT_STATE_C) { add_state(list, nl, s->out); add_state(list, nl, s->out1); return; }
  list[(*nl)++] = s;
}

static int is_match_list(State** list, int n) {
  for (int i = 0; i < n; ++i) if (list[i]->c == MATCH_STATE_C) return 1;
  return 0;
}

// Public entry — assembled NFA stored in `pool`, start state at pool[0]
// after first call.  This is a simple one-shot demo, not reentrant.

extern int32_t match(const char* pattern, int32_t plen,
                     const char* text,    int32_t tlen) {
  pool_n = 0; class_n = 0; listid = 0;
  State match_state = { MATCH_STATE_C, NULL, NULL, 0 };
  Parser P = { pattern, pattern + plen, 0 };
  State* start = parse_alt(&P);
  if (P.error || !start || P.p != P.end) return -1;
  // Patch all trailing out=NULL to match_state.
  for (int32_t i = 0; i < pool_n; ++i) {
    if (pool[i].c != SPLIT_STATE_C && pool[i].c != MATCH_STATE_C && !pool[i].out) {
      pool[i].out = &match_state;
    }
    if (pool[i].c == SPLIT_STATE_C) {
      if (!pool[i].out)  pool[i].out  = &match_state;
      if (!pool[i].out1) pool[i].out1 = &match_state;
    }
  }
  // Try substring match: anchor pattern at each starting offset.
  for (int32_t off = 0; off <= tlen; ++off) {
    listid++;
    int cn = 0, nn = 0;
    add_state(clist, &cn, start);
    for (int32_t i = off; i < tlen; ++i) {
      listid++;
      nn = 0;
      for (int k = 0; k < cn; ++k) {
        State* s = clist[k];
        int32_t code = s->c;
        char target = text[i];
        int hit = 0;
        if (code >= 0) hit = (code == (int32_t)(unsigned char)target);
        else if (code == -200) hit = 1;             // .
        else if (code == -201 || code == -202) {
          // Anchors are zero-width; just propagate the state.
          add_state(nlist, &nn, s->out);
          continue;
        }
        else hit = matches_class(code, target);
        if (hit) add_state(nlist, &nn, s->out);
      }
      // swap
      State** tmp_list = (State**)clist; (void)tmp_list;
      for (int j = 0; j < nn; ++j) clist[j] = nlist[j];
      cn = nn;
      if (is_match_list(clist, cn)) return 1;
      if (cn == 0) break;
    }
    if (is_match_list(clist, cn)) return 1;
  }
  return 0;
}
