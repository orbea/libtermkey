#include "termkey.h"
#include "termkey-internal.h"

#include <term.h>
#include <curses.h>

#include <ctype.h>
#include <stdio.h>
#include <string.h>

struct ti_keyinfo {
  const char *seq;
  size_t seqlen; // cached strlen of seq since we'll use it lots
  struct keyinfo key;
};

typedef struct {
  termkey_t *tk;

  int nseqs;
  int alloced_seqs;
  struct ti_keyinfo *seqs;
} termkey_ti;

static int funcname2keysym(const char *funcname, termkey_type *typep, termkey_keysym *symp, int *modmask, int *modsetp);
static void register_seq(termkey_ti *ti, const char *seq, termkey_type type, termkey_keysym sym, int modmask, int modset);

static void *new_driver(termkey_t *tk)
{
  setupterm((char*)0, 1, (int*)0);

  termkey_ti *ti = malloc(sizeof *ti);

  ti->tk = tk;

  ti->alloced_seqs = 32; // We'll allocate more space if we need
  ti->nseqs = 0;

  ti->seqs = malloc(ti->alloced_seqs * sizeof(ti->seqs[0]));

  int i;
  for(i = 0; strfnames[i]; i++) {
    // Only care about the key_* constants
    const char *name = strfnames[i];
    if(strncmp(name, "key_", 4) != 0)
      continue;

    const char *value = tigetstr(strnames[i]);
    if(!value || value == (char*)-1)
      continue;

    termkey_type type;
    termkey_keysym sym;
    int mask = 0;
    int set  = 0;

    if(!funcname2keysym(strfnames[i] + 4, &type, &sym, &mask, &set))
      continue;

    register_seq(ti, value, type, sym, mask, set);
  }

  return ti;
}

static void free_driver(void *private)
{
}

#define CHARAT(i) (tk->buffer[tk->buffstart + (i)])

static termkey_result getkey(termkey_t *tk, termkey_key *key, int force)
{
  termkey_ti *ti = tk->driver_info;

  if(tk->buffcount == 0)
    return tk->is_closed ? TERMKEY_RES_EOF : TERMKEY_RES_NONE;

  // Now we're sure at least 1 byte is valid
  unsigned char b0 = CHARAT(0);

  int i;
  for(i = 0; i < ti->nseqs; i++) {
    struct ti_keyinfo *s = &(ti->seqs[i]);

    if(s->seq[0] != b0)
      continue;

    if(tk->buffcount >= s->seqlen) {
      if(strncmp(s->seq, (const char*)tk->buffer + tk->buffstart, s->seqlen) == 0) {
        key->type      = s->key.type;
        key->code.sym  = s->key.sym;
        key->modifiers = s->key.modifier_set;
        (*tk->method.eat_bytes)(tk, s->seqlen);
        return TERMKEY_RES_KEY;
      }
    }
    else if(!force) {
      // Maybe we'd get a partial match
      if(strncmp(s->seq, (const char*)tk->buffer + tk->buffstart, tk->buffcount) == 0)
        return TERMKEY_RES_AGAIN;
    }
  }

  // No special seq. Must be a simple key then
  return (*tk->method.getkey_simple)(tk, key);
}

static struct {
  const char *funcname;
  termkey_type type;
  termkey_keysym sym;
  int mods;
} funcs[] =
{
  { "backspace", TERMKEY_TYPE_KEYSYM, TERMKEY_SYM_BACKSPACE, 0 },
  { "begin",     TERMKEY_TYPE_KEYSYM, TERMKEY_SYM_BEGIN,     0 },
  { "btab",      TERMKEY_TYPE_KEYSYM, TERMKEY_SYM_TAB,       TERMKEY_KEYMOD_SHIFT },
  { "dc",        TERMKEY_TYPE_KEYSYM, TERMKEY_SYM_DELETE,    0 },
  { "down",      TERMKEY_TYPE_KEYSYM, TERMKEY_SYM_DOWN,      0 },
  { "end",       TERMKEY_TYPE_KEYSYM, TERMKEY_SYM_END,       0 },
  { "find",      TERMKEY_TYPE_KEYSYM, TERMKEY_SYM_FIND,      0 },
  { "home",      TERMKEY_TYPE_KEYSYM, TERMKEY_SYM_HOME,      0 },
  { "ic",        TERMKEY_TYPE_KEYSYM, TERMKEY_SYM_INSERT,    0 },
  { "left",      TERMKEY_TYPE_KEYSYM, TERMKEY_SYM_LEFT,      0 },
  { "next",      TERMKEY_TYPE_KEYSYM, TERMKEY_SYM_PAGEDOWN,  0 }, // Not quite, but it's the best we can do
  { "npage",     TERMKEY_TYPE_KEYSYM, TERMKEY_SYM_PAGEDOWN,  0 },
  { "ppage",     TERMKEY_TYPE_KEYSYM, TERMKEY_SYM_PAGEUP,    0 },
  { "previous",  TERMKEY_TYPE_KEYSYM, TERMKEY_SYM_PAGEUP,    0 }, // Not quite, but it's the best we can do
  { "right",     TERMKEY_TYPE_KEYSYM, TERMKEY_SYM_RIGHT,     0 },
  { "select",    TERMKEY_TYPE_KEYSYM, TERMKEY_SYM_SELECT,    0 },
  { "up",        TERMKEY_TYPE_KEYSYM, TERMKEY_SYM_UP,        0 },
  { NULL },
};

static int funcname2keysym(const char *funcname, termkey_type *typep, termkey_keysym *symp, int *modmaskp, int *modsetp)
{
  int i;
  for(i = 0; funcs[i].funcname; i++) {
    if(strcmp(funcname, funcs[i].funcname) == 0) {
      *typep    = funcs[i].type;
      *symp     = funcs[i].sym;
      *modmaskp = funcs[i].mods;
      *modsetp  = funcs[i].mods;
      return 1;
    }
  }

  if(funcname[0] == 'f' && isdigit(funcname[1])) {
    *typep = TERMKEY_TYPE_FUNCTION;
    *symp  = atoi(funcname + 1);
    return 1;
  }

  // Last-ditch attempt; maybe it's a shift key?
  if(funcname[0] == 's' && funcname2keysym(funcname + 1, typep, symp, modmaskp, modsetp)) {
    *modmaskp |= TERMKEY_KEYMOD_SHIFT;
    *modsetp  |= TERMKEY_KEYMOD_SHIFT;
    return 1;
  }

  printf("TODO: Need to convert funcname %s to a type/sym\n", funcname);
  return 0;
}

static void register_seq(termkey_ti *ti, const char *seq, termkey_type type, termkey_keysym sym, int modmask, int modset)
{
  if(ti->nseqs == ti->alloced_seqs) {
    ti->alloced_seqs *= 2;
    void *newseqs = realloc(ti->seqs, ti->alloced_seqs * sizeof(ti->seqs[9]));
    // TODO: Error handle
    ti->seqs = newseqs;
  }

  int i = ti->nseqs++;
  ti->seqs[i].seq = seq;
  ti->seqs[i].seqlen = strlen(seq);
  ti->seqs[i].key.type = type;
  ti->seqs[i].key.sym = sym;
  ti->seqs[i].key.modifier_mask = modmask;
  ti->seqs[i].key.modifier_set  = modset;
}

struct termkey_driver termkey_driver_ti = {
  .new_driver  = new_driver,
  .free_driver = free_driver,

  .getkey = getkey,
};
