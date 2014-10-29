/* PANDABEGINCOMMENT
 *
 * Authors:
 *  Tim Leek               tleek@ll.mit.edu
 *  Ryan Whelan            rwhelan@ll.mit.edu
 *  Joshua Hodosh          josh.hodosh@ll.mit.edu
 *  Michael Zhivich        mzhivich@ll.mit.edu
 *  Brendan Dolan-Gavitt   brendandg@gatech.edu
 *
 * This work is licensed under the terms of the GNU GPL, version 2.
 * See the COPYING file in the top-level directory.
 *
PANDAENDCOMMENT */

#include <stdio.h>
#include "my_mem.h"
#include "my_bool.h"
#include "bitvector_label_set.cpp"
#include "shad_dir_32.h"
#include "shad_dir_64.h"
#include "max.h"
#include "guestarch.h"
#include "taint_processor.h"
#include "panda_memlog.h"
#include "network.h"
#include "panda_stats.h"
#include "panda_plugin_plugin.h"

#define SB_INLINE inline

#ifdef TAINTSTATS
// Bool for whether or not the function operates on tainted data
uint8_t taintedfunc;
#endif

/* XXX: Note, there is currently only support for copying taint into a new stack
 * frame, i.e. through tp_copy() and tp_delete(), not anything else.
 */

// Global count of taint labels
int count = 0;

int tainted_pointer = 1;

// stuff for control flow in traces
int next_step;
int previous_branch; // keep a history of 1
int taken_branch;


// if set, then we delete taint on dest instead of compute
int compute_is_delete = 0;

uint32_t max_taintset_card = 0;
uint32_t max_taintset_compute_number = 0;

uint32_t max_ref_count = 0;

extern "C" {

// Label all incoming network traffic as tainted
bool taint_label_incoming_network_traffic = 0;

// Query all outgoing network traffic for taint
bool taint_query_outgoing_network_traffic = 0;

// Taint labeling mode
int taint_label_mode = TAINT_BYTE_LABEL;

int tainted_instructions = 0;

// prototypes for on_load and on_store callback registering
PPP_PROT_REG_CB(on_load);
PPP_PROT_REG_CB(on_store);
PPP_PROT_REG_CB(before_execute_taint_ops);
PPP_PROT_REG_CB(after_execute_taint_ops);


// this adds the actual callback machinery including
// functions for registering callbacks
PPP_CB_BOILERPLATE(on_load);
PPP_CB_BOILERPLATE(on_store);
PPP_CB_BOILERPLATE(before_execute_taint_ops);
PPP_CB_BOILERPLATE(after_execute_taint_ops);
}

void tp_ls_iter(Shad *shad, Addr *a, int (*app)(uint32_t el, void *stuff1), void *stuff2);



Addr make_haddr(uint64_t a) {
  Addr ha;
  ha.typ = HADDR;
  ha.val.ha = a;
  ha.off = 0;
  ha.flag = (AddrFlag) 0;
  return ha;
}

Addr make_maddr(uint64_t a) {
  Addr ma;
  ma.typ = MADDR;
  ma.val.ma = a;
  ma.off = 0;
  ma.flag = (AddrFlag) 0;
  return ma;
}

Addr make_iaddr(uint64_t a) {
  Addr ia;
  ia.typ = IADDR;
  ia.val.ia = a;
  ia.off = 0;
  ia.flag = (AddrFlag) 0;
  return ia;
}

Addr make_paddr(uint64_t a) {
  Addr pa;
  pa.typ = PADDR;
  pa.val.pa = a;
  pa.off = 0;
  pa.flag = (AddrFlag) 0;
  return pa;
}

// if addr is one of HAddr, MAddr, IAddr, PAddr, LAddr, then add this offset to it
// else throw up
static Addr addr_add(Addr a, uint32_t o) {
  switch (a.typ) {
  case HADDR:
    a.val.ha += o;
    break;
  case MADDR:
    a.val.ma += o;
    break;
  case IADDR:
    a.val.ia += o;
    break;
  case PADDR:
    a.val.pa += o;
    break;
  default:
    // Thou shalt not.
    printf ("You called addr_add with an Addr other than HADDR, MADDR, IADDR, PADDR.  That isn't meaningful.\n");
    assert (1==0);
    break;
  }
  return a;
}


// increment addr a in place
static void addr_inc(Addr *a) {
  switch (a->typ) {
  case HADDR:
    a->val.ha++;
    break;
  case MADDR:
    a->val.ma++;
    break;
  case IADDR:
    a->val.ia++;
    break;
  case PADDR:
    a->val.pa++;
    break;
  default:
    // Thou shalt not.
    printf ("You called addr_add with an Addr other than HADDR, MADDR, IADDR, PADDR.  That isn't meaningful.\n");
    assert (1==0);
    break;
  }
}



//SB_INLINE uint8_t get_ram_bit(Shad *shad, uint32_t addr) {
uint8_t get_ram_bit(Shad *shad, uint32_t addr) {
    uint8_t taint_byte = shad->ram_bitmap[addr >> 3];
    return (taint_byte & (1 << (addr & 7)));
}


static SB_INLINE void set_ram_bit(Shad *shad, uint32_t addr) {
    uint8_t taint_byte = shad->ram_bitmap[addr >> 3];
    taint_byte |= (1 << (addr & 7));
    shad->ram_bitmap[addr >> 3] = taint_byte;
}


static SB_INLINE void clear_ram_bit(Shad *shad, uint32_t addr) {
    uint8_t taint_byte = shad->ram_bitmap[addr >> 3];
    taint_byte &= (~(1 << (addr & 7)));
    shad->ram_bitmap[addr >> 3] = taint_byte;
}

// Apply taint to a buffer of memory
void add_taint_ram(CPUState *env, Shad *shad, TaintOpBuffer *tbuf,
        uint64_t addr, int length){
    struct addr_struct a = {};
    a.typ = MADDR;
    struct taint_op_struct op = {};
    op.typ = LABELOP;
    for (int i = 0; i < length; i++){
#ifdef CONFIG_SOFTMMU
        target_phys_addr_t pa = cpu_get_phys_addr(env, addr + i);
        if (pa == (target_phys_addr_t)(-1)) {
            printf("can't label addr=0x%lx: mmu hasn't mapped virt->phys, i.e., it isnt actually there.\n", addr +i);
            continue;
        }
        assert (pa != -1);
        a.val.ma = pa;
#else
        a.val.ma = addr + i;
#endif // CONFIG_SOFTMMU
        op.val.label.a = a;
        if (taint_label_mode == TAINT_BYTE_LABEL){
            op.val.label.l = i + count;
        }
        else if (taint_label_mode == TAINT_BINARY_LABEL){
            op.val.label.l = 1;
        }
        tob_op_write(tbuf, &op);	
    }
    assert (tbuf->ptr <= (tbuf->start + tbuf->max_size));
    tob_process(tbuf, shad, NULL);
    count += length;
}

// Apply taint to a buffer of IO memory
void add_taint_io(CPUState *env, Shad *shad, TaintOpBuffer *tbuf,
        uint64_t addr, int length){
    Addr a = make_iaddr(addr);
    struct taint_op_struct op = {};
    op.typ = LABELOP;
    for (int i = 0; i < length; i++){
        a.val.ia = addr + i;
        op.val.label.a = a;
        if (taint_label_mode == TAINT_BYTE_LABEL){
            op.val.label.l = i + count;
        }
        else if (taint_label_mode == TAINT_BINARY_LABEL){
            op.val.label.l = 1;
        }
        // make the taint op buffer bigger if necessary
        tob_resize(&tbuf);
        tob_op_write(tbuf, &op);	
    }
}

/*
   Initialize the shadow memory for taint processing.
   hd_size -- size of hd in bytes
   mem_size -- size of ram in bytes
   io_size -- max address an io buffer address can be
   max_vals -- max number of numbered llvm values we'll need
 */
Shad *tp_init(uint64_t hd_size, uint32_t mem_size, uint64_t io_size,
        uint32_t max_vals) {
    //    Shad *shad = (Shad *) my_malloc(sizeof(Shad), poolid_taint_processor);
    void *tmp = my_malloc(sizeof(Shad), poolid_taint_processor);
    Shad *shad = new(tmp) Shad;
    shad->hd_size = hd_size;
    shad->mem_size = mem_size;
    shad->io_size = io_size;
    shad->port_size = 0xffff * 4; // assume a max port size of 4 bytes,
        // and 0xffff max ports according to Intel manual
    shad->num_vals = max_vals;
    shad->guest_regs = NUMREGS;
    shad->hd = shad_dir_new_64(12,12,16);
    shad->ram = fast_shad_new(FAST_SHAD_BYTE);
    shad->io = shad_dir_new_64(12,12,16);
    shad->ports = shad_dir_new_32(10,10,12);

    // we're working with LLVM values that can be up to 128 bits
    shad->llv = fast_shad_new(max_vals * FUNCTIONFRAMES * MAXREGSIZE, FAST_SHAD_BYTE);
    shad->ret = fast_shad_new(MAXREGSIZE, FAST_SHAD_BYTE);
    // guest registers are generally the size of the guest architecture
    shad->grv = fast_shad_new(NUMREGS * WORDSIZE, FAST_SHAD_BYTE);

    // architecture-dependent size defined in guestarch.h
    if (NUMSPECADDRS){
        /*
         * +NUMREGS is necessary offset for how we process these according to
         * enums
         */
        shad->gsv = fast_shad_new(NUMSPECADDRS + NUMREGS, FAST_SHAD_BYTE);
    }
    else {
        shad->gsv = NULL;
    }
    shad->current_frame = 0;
    return shad;
}


/*
 * Delete a shadow memory
 */
void tp_free(Shad *shad){
    shad_dir_free_64(shad->hd);
    fast_shad_free(shad->ram);
    shad_dir_free_64(shad->io);
    shad_dir_free_32(shad->ports);
    fast_shad_free(shad->llv);
    fast_shad_free(shad->ret);
    fast_shad_free(shad->grv);
    if (shad->gsv){
        fast_shad_free(shad->gsv);
    }
    my_free(shad, sizeof(Shad), poolid_taint_processor);
}

/*
 * Remove all taint from all parts of the shadow memory
 */
void clear_shadow_memory(Shad **shad){
    uint64_t hd_size = (*shad)->hd_size;
    uint32_t mem_size = (*shad)->mem_size;
    uint64_t io_size = (*shad)->io_size;
    uint32_t num_vals = (*shad)->num_vals;
    tp_free(*shad);
    *shad = tp_init(hd_size, mem_size, io_size, num_vals);
}

// returns a copy of the labelset associated with a.  or NULL if none.
// so you'll need to call labelset_free on this pointer when done with it.
static SB_INLINE LabelSet *tp_labelset_get(Shad *shad, Addr *a) {
    LabelSet *ls = NULL;
    switch (a->typ) {
        case HADDR:
            {
                ls = shad_dir_find_64(shad->hd, a->val.ha+a->off);
                break;
            }
        case MADDR:
            {
                ls = fast_shad_query(shad->ram, a->val.ma+a->off);
                break;
            }
        case IADDR:
            {
                ls = shad_dir_find_64(shad->io, a->val.ia+a->off);
                break;
            }
        case PADDR:
            {
                ls = shad_dir_find_32(shad->ports, a->val.ia+a->off);
                break;
            }
        // multipliers are for register and stack frame indexing in shadow
        // register space
        case LADDR:
            {
                if (a->flag == FUNCARG){
                    assert((shad->current_frame + 1) < FUNCTIONFRAMES);
                    ls = fast_shad_query(
                        shad->llv, shad->num_vals*(shad->current_frame + 1) +
                                  a->val.la*MAXREGSIZE +
                                  a->off);
                }
                else {
                    assert(shad->current_frame < FUNCTIONFRAMES);
                    ls = fast_shad_query(
                        shad->llv, shad->num_vals*shad->current_frame +
                                  a->val.la*MAXREGSIZE +
                                  a->off);
                }
                break;
            }
        case GREG:

                ls = fast_shad_query(shad->grv, a->val.gr * WORDSIZE + a->off);
                break;
            }
        case GSPEC:
            {
                // SpecAddr enum is offset by the number of guest registers
                ls = fast_shad_query(shad->gsv, a->val.gs - NUMREGS + a->off);
                break;
            }
        case CONST:
            {
                ls = NULL;
                break;
            }
        case RET:
            {
                ls = fast_shad_query(shad->ret, a->off);
                break;
            }
        default:
            assert (1==0);
    }
    return ls;
}


// returns TRUE (1) iff a has a non-empty taint set
SB_INLINE uint8_t tp_query(Shad *shad, Addr *a) {
    assert (shad != NULL);
    LabelSet *ls = tp_labelset_get(shad, a);
    return !(labelset_is_empty(ls));
}



// returns label set cardinality
uint32_t tp_query_ram(Shad *shad, uint64_t pa) {
  Addr ra;
  ra.typ = MADDR;
  ra.val.ma = pa;
  ra.off = 0;
  ra.flag = (AddrFlag) 0;
  if (tp_query(shad, &ra)) {
    LabelSet *ls = tp_labelset_get(shad, &ra);    
    uint32_t c = labelset_card(ls);
    assert (c > 0);
    return c;
  }
  // not tainted
  return 0;
}



// returns label set cardinality
uint32_t tp_query_reg(Shad *shad, int reg_num, int offset) {
  Addr ra;
  ra.typ = GREG;
  ra.val.gr = reg_num;
  ra.off = offset;
  ra.flag = (AddrFlag) 0;
  if (tp_query(shad, &ra)) {
    LabelSet *ls = tp_labelset_get(shad, &ra);    
    uint32_t c = labelset_card(ls);
    assert (c > 0);
    return c;
  }
  // not tainted
  return 0;
}






//SB_INLINE void tp_ls_iter(Shad *shad, Addr *a, int (*app)(uint32_t el, void *stuff1), void *stuff2) {
void tp_ls_iter(Shad *shad, Addr *a, int (*app)(uint32_t el, void *stuff1), void *stuff2) {
  LabelSet *ls = tp_labelset_get(shad, a);
  if (!labelset_is_empty(ls)) {
    labelset_iter(ls, app, stuff2);
  }
}

void tp_ls_ram_iter(Shad *shad, uint64_t pa, int (*app)(uint32_t el, void *stuff1), void *stuff2) {
  Addr ra;
  ra.typ = MADDR;
  ra.val.ma = pa;
  ra.off = 0;
  ra.flag = (AddrFlag) 0;
  tp_ls_iter(shad, &ra, app, stuff2);
}


void tp_ls_reg_iter(Shad *shad, int reg_num, int offset, int (*app)(uint32_t el, void *stuff1), void *stuff2) {
  Addr ra;
  ra.typ = GREG; 
  ra.val.gr = reg_num;
  ra.off = offset;
  ra.flag = (AddrFlag) 0;
  tp_ls_iter(shad, &ra, app, stuff2);
}


// returns number of tainted addrs in ram
uint32_t tp_occ_ram(Shad *shad) {

  fflush(stdout);
  if (shad->ram) {
#ifdef TARGET_X86_64
    uint32_t x = shad_dir_occ_64(shad->ram);
#else
    uint32_t x = shad_dir_occ_32(shad->ram);
#endif
    return x;
  }
  else {
    return 0;
  }
}



// untaint -- discard label set associated with a
SB_INLINE void tp_delete(Shad *shad, Addr *a) {

    assert (shad != NULL);
    switch (a->typ) {
        case HADDR:
            // NB: just returns if nothing there
            shad->taint_state_changed |= shad_dir_mem_64(shad->hd, a->val.ha+a->off); 
            shad_dir_remove_64(shad->hd, a->val.ha+a->off);
            break;
        case MADDR:
            fast_shad_set(shad->ram, a->val.ma+a->off);
            break;
        case IADDR:
            shad_dir_remove_64(shad->io, a->val.ia+a->off);
            break;
        case PADDR:
            shad_dir_remove_32(shad->ports, a->val.ia+a->off);
            break;
        case LADDR:
            if (a->flag == FUNCARG){
                fast_shad_remove(shad->llv, shad->num_vals*(shad->current_frame + 1) +
                        a->val.la*MAXREGSIZE +
                        a->off);
            }
            else {
                fast_shad_remove(shad->llv, shad->num_vals*shad->current_frame +
                        a->val.la*MAXREGSIZE +
                        a->off);
            }
            break;
        case GREG:
            fast_shad_remove(shad->grv, a->val.gr * WORDSIZE + a->off);
            break;
        case GSPEC:
            fast_shad_remove(shad->gsv, a->val.gs - NUMREGS + a->off);
            break;
        case RET:
            fast_shad_remove(shad->ret, a->off);
            break;
        default:
            assert (1==0);
    }
}


// here we are storing a copy of ls in the shadow memory.
// so ls is caller's to free
static SB_INLINE void tp_labelset_put(Shad *shad, Addr *a, LabelSet *ls) {
    switch (a->typ) {
        case HADDR:
            shad_dir_add_64(shad->hd, a->val.ha + a->off, ls);
#ifdef TAINTDEBUG
            printf("Labelset put on HD: 0x%lx\n", (uint64_t)(a->val.ha + a->off));
            labelset_spit(ls);
            printf("\n");
#endif
            break;
        case MADDR:
            fast_shad_set(shad->ram, a->val.ma + a->off, ls);
#ifdef TAINTDEBUG
            printf("Labelset put in RAM: 0x%lx\n", (uint64_t)(a->val.ma + a->off));
            labelset_spit(ls);
            printf("\n");
#endif
            break;
        case IADDR:
#ifdef TAINTDEBUG
            printf("Labelset put in IO: 0x%lx\n", (uint64_t)(a->val.ia + a->off));
            labelset_spit(ls);
            printf("\n");
#endif
            shad_dir_add_64(shad->io, a->val.ia + a->off, ls);
            break;
        case PADDR:
#ifdef TAINTDEBUG
            printf("Labelset put in port: 0x%lx\n", (uint64_t)(a->val.pa + a->off));
            labelset_spit(ls);
            printf("\n");
#endif
            shad_dir_add_32(shad->ports, a->val.pa + a->off, ls);
            break;
        case LADDR:
#ifdef TAINTDEBUG
            printf("Labelset put in LA: 0x%lx\n", (uint64_t)(a->val.la+a->off));
            labelset_spit(ls);
            printf("\n");
#endif
            if (a->flag == FUNCARG){
                // put in new function frame
                assert((shad->current_frame + 1) < FUNCTIONFRAMES);
                fast_shad_set(shad->llv, shad->num_vals*(shad->current_frame + 1) +
                    a->val.la*MAXREGSIZE +
                    a->off, ls_copy);
            }
            else {
                assert(shad->current_frame < FUNCTIONFRAMES);
                fast_shad_set(shad->llv, shad->num_vals*shad->current_frame +
                    a->val.la*MAXREGSIZE +
                    a->off, ls_copy);
            }
            break;
        case GREG:
#ifdef TAINTDEBUG
            printf("Labelset put in GR: 0x%lx\n", (uint64_t)(a->val.gr+a->off));
            labelset_spit(ls);
            printf("\n");
#endif
            // need to call labelset_copy to increment ref count
            fast_shad_set(shad->grv, a->val.gr * WORDSIZE + a->off, ls);
            break;
        case GSPEC:
#ifdef TAINTDEBUG
            printf("Labelset put in GS: 0x%lx\n", (uint64_t)(a->val.gs+a->off));
            labelset_spit(ls);
            printf("\n");
#endif
            // SpecAddr enum is offset by the number of guest registers
            fast_shad_set(shad->gsv, a->val.gs - NUMREGS + a->off, ls);
            break;
        case RET:
#ifdef TAINTDEBUG
            printf("Labelset put in ret\n");
            labelset_spit(ls);
            printf("\n");
#endif
            fast_shad_set(shad->ret, a->off, ls);
            break;
        default:
            assert (1==0);
    }
}

SB_INLINE void addr_spit(Addr *a) {
  switch (a->typ) {
  case HADDR:    printf ("(h%lx", a->val.ha);    break;
  case MADDR:    printf ("(m%lx", a->val.ma);    break;
  case IADDR:    printf ("(i%lx", a->val.ia);    break;
  case PADDR:    printf ("(p%lx", a->val.pa);    break;
  case LADDR:    printf ("(l%lx", a->val.la);    break;
  case GREG:     printf ("(r%lx", a->val.gr);    break;
  case GSPEC:    printf ("(s%lx", a->val.gs);    break;
  case UNK:      printf ("(u");    break;
  case CONST:    printf ("(c");    break;
  case RET:      printf ("(r");    break;
  default: assert (1==0);
  }
  printf (",%d,%x)", a->off, a->flag);
}

    

// label -- associate label l with address a
SB_INLINE void tp_label(Shad *shad, Addr *a, Label l) {
    assert (shad != NULL);
    
    
    /*
    printf ("tp_label ");
    addr_spit(a);
    printf (" %d\n", l);
    */    

    LabelSet *ls = tp_labelset_get(shad, a);
    LabelSet *ls2 = label_set_api.label_set_singleton(l);
    LabelSet *result = label_set_api.label_set_union(ls, ls2);
    tp_labelset_put(shad, a, result);
}

void tp_label_ram(Shad *shad, uint64_t pa, uint32_t l) {
  Addr ra;
  ra.typ = MADDR;
  ra.val.ma = pa;
  ra.off = 0;
  ra.flag = (AddrFlag) 0;
  tp_label(shad, &ra, l);
}

void tp_delete_ram(Shad *shad, uint64_t pa) {

  Addr ra;
  ra.typ = MADDR;
  ra.val.ma = pa;
  ra.off = 0;
  ra.flag = (AddrFlag) 0;
  tp_delete(shad, &ra);
}



SB_INLINE uint8_t addrs_equal(Addr *a, Addr *b) {
    if (a->typ != b->typ)
        return FALSE;
    switch (a->typ) {
        case HADDR:
            return a->val.ha+a->off == b->val.ha+b->off;
        case MADDR:
            return a->val.ma+a->off == b->val.ma+b->off;
        case IADDR:
            return a->val.ia+a->off == b->val.ia+b->off;
        case PADDR:
            return a->val.pa+a->off == b->val.pa+b->off;
        case LADDR:
            return (a->val.la == b->val.la)
                   && (a->off == b->off)
                   && (a->flag == b->flag);
        case GREG:
            return (a->val.gr == b->val.gr) && (a->off == b->off);
        case GSPEC:
            return (a->val.gs == b->val.gs) && (a->off == b->off);
        case RET:
            return (a->off == b->off);
        default:
            assert (1==0);
            return 0;
    }
    return FALSE;
}



void fprintf_addr(Shad *shad, Addr *a, FILE *fp) {
  uint32_t current_frame;
  switch(a->typ) {
  case HADDR:
    fprintf(fp,"h0x%llx", (long long unsigned int) a->val.ha+a->off);
    break;
  case MADDR:
    fprintf(fp,"m0x%llx", (long long unsigned int) a->val.ma+a->off);
    break;
  case IADDR:
    fprintf(fp,"i0x%llx", (long long unsigned int) a->val.ia+a->off);
    break;
  case PADDR:
    fprintf(fp,"p0x%llx", (long long unsigned int) a->val.pa+a->off);
    break;
  case LADDR:
    if (!shad){
      current_frame = 0; // not executing taint ops, assume frame 0
    }
    else {
      current_frame = shad->current_frame;
    }

    if (a->flag == FUNCARG){
      fprintf(fp,"[%d]l%lld[%d]", current_frame + 1,
	      (long long unsigned int) a->val.la, a->off);
    }
    else {
      fprintf(fp,"[%d]l%lld[%d]", current_frame,
	      (long long unsigned int) a->val.la, a->off);
    }
    break;
  case GREG:
    fprintf_reg(a, fp);
    break;
  case GSPEC:
    fprintf_spec(a, fp);
    break;
  case UNK:
    if (a->flag == IRRELEVANT){
      fprintf(fp,"irrelevant");
    }
    //else if (a->flag == READLOG) {
    else if (a->typ == UNK){ 
      fprintf(fp,"unknown");
    }
    else {
      assert(1==0);
    }
    break;
  case CONST:
    fprintf(fp,"constant");
    break;
  case RET:
    fprintf(fp,"ret[%d]", a->off);
    break;
  default:
    assert (1==0);
  }
}

void print_addr(Shad *shad, Addr *a) {
  fprintf_addr(shad, a, stdout);
}
 
/*
void print_addr(Shad *shad, Addr *a) {
    uint32_t current_frame;
    switch(a->typ) {
        case HADDR:
            printf ("h0x%llx", (long long unsigned int) a->val.ha+a->off);
            break;
        case MADDR:
            printf ("m0x%llx", (long long unsigned int) a->val.ma+a->off);
            break;
        case IADDR:
            printf ("i0x%llx", (long long unsigned int) a->val.ia+a->off);
            break;
        case PADDR:
            printf ("p0x%llx", (long long unsigned int) a->val.pa+a->off);
            break;
        case LADDR:
            if (!shad){
                current_frame = 0; // not executing taint ops, assume frame 0
            }
            else {
                current_frame = shad->current_frame;
            }

            if (a->flag == FUNCARG){
                printf ("[%d]l%lld[%d]", current_frame + 1,
                    (long long unsigned int) a->val.la, a->off);
            }
            else {
                printf ("[%d]l%lld[%d]", current_frame,
                    (long long unsigned int) a->val.la, a->off);
            }
            break;
        case GREG:
            printreg(a);
            break;
        case GSPEC:
            printspec(a);
            break;
        case UNK:
            if (a->flag == IRRELEVANT){
                printf("irrelevant");
            }
            //else if (a->flag == READLOG) {
            else if (a->typ == UNK){
                printf("unknown");
            }
            else {
                assert(1==0);
            }
            break;
        case CONST:
            printf("constant");
            break;
        case RET:
            printf("ret[%d]", a->off);
            break;
        default:
            assert (1==0);
    }
}
*/


// copy -- b gets whatever label set is currently associated with a
SB_INLINE void tp_copy(Shad *shad, Addr *a, Addr *b) {

    assert (shad != NULL);
    //assert (!(addrs_equal(a,b)));
    if (addrs_equal(a, b)) return;
    LabelSet *ls_a = tp_labelset_get(shad, a);
    tb_labelset_put(shad, b, ls_a);
}


// compute -- c gets union of label sets currently associated with a and b
// delete previous association
SB_INLINE void tp_compute(Shad *shad, Addr *a, Addr *b, Addr *c) {
    assert (shad != NULL);    

    // we want the possibilities of address equality for unioning
    //assert (!(addrs_equal(a,b)));
    //assert (!(addrs_equal(b,c)));
    //assert (!(addrs_equal(a,c)));
    LabelSet *ls_a = tp_labelset_get(shad, a);
    LabelSet *ls_b = tp_labelset_get(shad, b);
    LabelSet *ls_c = label_set_api.label_set_union(ls_a, ls_b);
    tp_labelset_put(shad, c, ls_c);
#ifdef TAINTDEBUG
    if (!labelset_is_empty(ls_c)){
        printf("labelset c: ");
        labelset_spit(tp_labelset_get(shad, c));
        printf("\n");
    }
#endif
}
//


void fprintf_tob_op(Shad *shad, TaintOp *op, FILE *fp) {
  switch (op->typ) {
  case LABELOP:
    {
      fprintf(fp,"label ");
      fprintf_addr(shad, &(op->val.label.a), fp);
      fprintf(fp," %d\n", op->val.label.l);
      break;
    }
  case DELETEOP:
    {
      fprintf(fp,"delete ");
      fprintf_addr(shad, &(op->val.deletel.a), fp);
      fprintf(fp,"\n");
      break;
    }
  case COPYOP:
    {
      fprintf(fp,"copy ");
      fprintf_addr(shad, &(op->val.copy.a), fp);
      fprintf(fp," ");
      fprintf_addr(shad, &(op->val.copy.b), fp);
      fprintf(fp,"\n");
      break;
    }
  case BULKCOPYOP:
    {
      fprintf(fp,"bulk copy ");
      fprintf_addr(shad, &(op->val.bulkcopy.a), fp);
      fprintf(fp," ");
      fprintf_addr(shad, &(op->val.bulkcopy.b), fp);
      fprintf(fp,"Len: %u\n", op->val.bulkcopy.l);
      break;
    }
  case COMPUTEOP:
    {
      fprintf(fp,"compute ");
      fprintf_addr(shad, &(op->val.compute.a), fp);
      fprintf(fp," ");
      fprintf_addr(shad, &(op->val.compute.b), fp);
      fprintf(fp," ");
      fprintf_addr(shad, &(op->val.compute.c), fp); 
      fprintf(fp,"\n");
      break;
    }
  case INSNSTARTOP:
    {
      fprintf(fp,"insn_start: %s, %d ops\n", op->val.insn_start.name,
	      op->val.insn_start.num_ops);
      break;
    } 
  case PCOP:
    {
      fprintf(fp,"pc op: %lx\n", op->val.pc);
      break;
    }
 case LDCALLBACKOP:
    {
      fprintf(fp,"ldcallback\n");
      break;
    }

  case STCALLBACKOP:
    {
      fprintf(fp,"stcallback\n");
      break;
    }



  case CALLOP:
    {
      fprintf(fp,"call %s\n", op->val.call.name);
      break;
    }
  case RETOP:
    {
      fprintf(fp,"return\n");
      break;
    }
  default:
    printf ("hmm.  i see a taint op i dont recognize. type is %d\n", op->typ);

    assert (1==0);
  }
}

void tob_op_print(Shad *shad, TaintOp *op) {
  fprintf_tob_op(shad, op, stdout);
} 
 
SB_INLINE void process_insn_start_op(TaintOp *op, TaintOpBuffer *buf,
        DynValBuffer *dynval_buf){
#ifdef TAINTDEBUG
    printf("Fixing up taint op buffer for: %s\n", op->val.insn_start.name);
#endif

    DynValEntry dventry;
    if(op->val.insn_start.flag == INSNREADLOG) {
      // Make sure there is still something to read in the buffer
      assert(((uintptr_t)(dynval_buf->ptr) - (uintptr_t)(dynval_buf->start))
          < dynval_buf->cur_size);

      read_dynval_buffer(dynval_buf, &dventry);

      if (dventry.entrytype == EXCEPTIONENTRY){
          //printf("EXCEPTION FOUND IN DYNAMIC LOG\n");
          next_step = EXCEPT;
          return;
      }
    }

    if (!strcmp(op->val.insn_start.name, "load")){

        // Check to see if it's either a regular load or port load
        if (((dventry.entrytype != ADDRENTRY) && (dventry.entrytype != PADDRENTRY))
                || ((dventry.entry.memaccess.op != LOAD)
                    && (dventry.entry.memaccess.op != PLOAD))){
            fprintf(stderr, "Error: dynamic log doesn't align\n");
            fprintf(stderr, "In: load\n");
            exit(1);
        }

        else if ((dventry.entrytype == ADDRENTRY)
                && (dventry.entry.memaccess.op == LOAD)) {
            /*** Fix up taint op buffer here ***/
            char *saved_buf_ptr = buf->ptr;
            TaintOp *cur_op = (TaintOp*) buf->ptr;

            int i;
            for (i = 0; i < op->val.insn_start.num_ops; i++){

                switch (cur_op->typ){
                    case COPYOP:
                        if (dventry.entry.memaccess.addr.flag == IRRELEVANT){
                            // load from irrelevant part of CPU state
                            // delete taint at the destination
                            cur_op->val.copy.a.flag = IRRELEVANT;
                        }
                        else if (dventry.entry.memaccess.addr.typ == GREG){
                            // guest register
                            cur_op->val.copy.a.flag = (AddrFlag) 0;
                            cur_op->val.copy.a.typ = GREG;
                            cur_op->val.copy.a.val.gr =
                                dventry.entry.memaccess.addr.val.gr;
                        }
                        else if (dventry.entry.memaccess.addr.typ == GSPEC){
                            // guest special address
                            cur_op->val.copy.a.flag = (AddrFlag) 0;
                            cur_op->val.copy.a.typ = GSPEC;
                            cur_op->val.copy.a.val.gs =
                                dventry.entry.memaccess.addr.val.gs;

                        }
                        else if (dventry.entry.memaccess.addr.typ == MADDR){
                            // guest RAM
                            cur_op->val.copy.a.flag = (AddrFlag) 0;
                            cur_op->val.copy.a.typ = MADDR;
                            cur_op->val.copy.a.val.ma =
                                dventry.entry.memaccess.addr.val.ma;
                        }
                        else {
                            assert(1==0);
                        }
                        break;
		    case LDCALLBACKOP:
		      {
			if (ppp_on_load_num_cb > 0) {
			  if (dventry.entry.memaccess.addr.typ == MADDR) {
			    // load callback.  fill in the address
			    cur_op->val.ldcallback.a.flag = (AddrFlag) 0;
			    cur_op->val.ldcallback.a.typ = MADDR;
			    cur_op->val.ldcallback.a.val.ma = 
			      dventry.entry.memaccess.addr.val.ma;
			  }
			}
			break;
		      }
		    case STCALLBACKOP:
		      // really shouldnt happen
		      assert (1==0);		      
                    default:
                        // taint ops for load only consist of copy ops
                        assert(1==0);
                }

                cur_op++;
            }

            buf->ptr = saved_buf_ptr;
        }

        else if ((dventry.entrytype == PADDRENTRY)
                && (dventry.entry.portaccess.op == PLOAD)) {
            /*** Fix up taint op buffer here ***/
            char *saved_buf_ptr = buf->ptr;
            TaintOp *cur_op = (TaintOp*) buf->ptr;

            int i;
            for (i = 0; i < op->val.insn_start.num_ops; i++){
                // For port accesses, we assume that data is only moved between
                // ports and guest registers.  If this assumption is wrong then
                // we will fail here.
                switch (cur_op->typ){
                    case COPYOP:
                        if (dventry.entry.memaccess.addr.typ == PADDR){
                            // guest register
                            cur_op->val.copy.a.flag = (AddrFlag) 0;
                            cur_op->val.copy.a.typ = PADDR;
                            cur_op->val.copy.a.val.pa =
                                dventry.entry.portaccess.addr.val.pa;
                        }
                        else {
                            assert(1==0);
                        }
                        break;

                    default:
                        // taint ops for port load only consist of copy ops
                        assert(1==0);
                }

                cur_op++;
            }

            buf->ptr = saved_buf_ptr;
        }


        else {
            fprintf(stderr, "Error: unknown error in dynamic log\n");
            fprintf(stderr, "In: load\n");
            exit(1);
        }
    }

    else if (!strcmp(op->val.insn_start.name, "store")){

        // Check to see if it's either a regular store or port store
        if (((dventry.entrytype != ADDRENTRY) && (dventry.entrytype != PADDRENTRY))
                || ((dventry.entry.memaccess.op != STORE)
                    && (dventry.entry.memaccess.op != PSTORE))){
            fprintf(stderr, "Error: dynamic log doesn't align\n");
            fprintf(stderr, "In: store\n");
            exit(1);
        }

        else if ((dventry.entrytype == ADDRENTRY)
                && (dventry.entry.memaccess.op == STORE)) {
            /*** Fix up taint op buffer here ***/
            char *saved_buf_ptr = buf->ptr;
            TaintOp *cur_op = (TaintOp*) buf->ptr;

            int i;
            for (i = 0; i < op->val.insn_start.num_ops; i++){

                switch (cur_op->typ){
                    case COPYOP:
                        if (dventry.entry.memaccess.addr.flag == IRRELEVANT){
                            // store to irrelevant part of CPU state
                            // delete taint at the destination
                            cur_op->val.copy.b.flag = IRRELEVANT;
                        }
                        else if (dventry.entry.memaccess.addr.typ == GREG){
                            // guest register
                            cur_op->val.copy.b.flag = (AddrFlag) 0;
                            cur_op->val.copy.b.typ = GREG;
                            cur_op->val.copy.b.val.gr =
                                dventry.entry.memaccess.addr.val.gr;
                        }
                        else if (dventry.entry.memaccess.addr.typ == GSPEC){
                            // guest special address
                            cur_op->val.copy.b.flag = (AddrFlag) 0;
                            cur_op->val.copy.b.typ = GSPEC;
                            cur_op->val.copy.b.val.gs =
                                dventry.entry.memaccess.addr.val.gs;
                        }
                        else if (dventry.entry.memaccess.addr.typ == MADDR){
                            // guest RAM
                            cur_op->val.copy.b.flag = (AddrFlag) 0;
                            cur_op->val.copy.b.typ = MADDR;
                            cur_op->val.copy.b.val.ma =
                                dventry.entry.memaccess.addr.val.ma;
                        }
                        else {
                            assert(1==0);
                        }
                        break;

		    case STCALLBACKOP:
		      {
			if (ppp_on_store_num_cb > 0) {
			  if (dventry.entry.memaccess.addr.typ == MADDR){
			    cur_op->val.stcallback.a.flag = (AddrFlag) 0;
			    cur_op->val.stcallback.a.typ = MADDR;
			    cur_op->val.stcallback.a.val.ma =
			      dventry.entry.memaccess.addr.val.ma;
			  }
			}
			break;
		      }     
		      
                    /* this only assumes we are in tainted pointer mode,
                     * with the associated taint models
                     */
                    case COMPUTEOP:
		      
		      if (tainted_pointer) {
		      
                        if (dventry.entry.memaccess.addr.flag == IRRELEVANT){
                            // store to irrelevant part of CPU state
                            // delete taint at the destination
                            cur_op->val.compute.b.flag = IRRELEVANT;
                            cur_op->val.compute.c.flag = IRRELEVANT;
                        }

                        // for store, if B and C aren't of type UNK, then
                        // skip over them (see the taint model, and how we
                        // use RET as a temp register)
                        else if (cur_op->val.compute.b.typ != UNK
                                && cur_op->val.compute.c.typ != UNK){
                            // do nothing
                        }
                        else if (dventry.entry.memaccess.addr.typ == GREG){
                            // guest register
                            // a register should never be a tainted pointer,
                            // so this is ignored in tob_process()
                            cur_op->val.compute.b.flag = (AddrFlag) 0;
                            cur_op->val.compute.b.typ = GREG;
                            cur_op->val.compute.b.val.gr =
                                dventry.entry.memaccess.addr.val.gr;
                            cur_op->val.compute.c.flag = (AddrFlag) 0;
                            cur_op->val.compute.c.typ = GREG;
                            cur_op->val.compute.c.val.gr =
                                dventry.entry.memaccess.addr.val.gr;
                        }
                        else if (dventry.entry.memaccess.addr.typ == GSPEC){
                            // special address
                            // a register should never be a tainted pointer,
                            // so this is ignored in tob_process()
                            cur_op->val.compute.b.flag = (AddrFlag) 0;
                            cur_op->val.compute.b.typ = GSPEC;
                            cur_op->val.compute.b.val.gs =
                                dventry.entry.memaccess.addr.val.gs;
                            cur_op->val.compute.c.flag = (AddrFlag) 0;
                            cur_op->val.compute.c.typ = GSPEC;
                            cur_op->val.compute.c.val.gs =
                                dventry.entry.memaccess.addr.val.gs;
                        }
                        else if (dventry.entry.memaccess.addr.typ == MADDR){
                            // guest RAM
                            cur_op->val.compute.b.flag = (AddrFlag) 0;
                            cur_op->val.compute.b.typ = MADDR;
                            cur_op->val.compute.b.val.ma =
                                dventry.entry.memaccess.addr.val.ma;
                            cur_op->val.compute.c.flag = (AddrFlag) 0;
                            cur_op->val.compute.c.typ = MADDR;
                            cur_op->val.compute.c.val.ma =
                                dventry.entry.memaccess.addr.val.ma;
                        }
                        else {
                            assert(1==0);
                        }
		      } // tainted_pointer on
			
                        break;

                    case DELETEOP:
                        if (dventry.entry.memaccess.addr.flag == IRRELEVANT){
                            // do nothing for delete at address we aren't
                            // tracking
                            cur_op->val.deletel.a.flag = IRRELEVANT;
                        }
                        else if (dventry.entry.memaccess.addr.typ == GREG){
                            // guest register
                            cur_op->val.deletel.a.flag = (AddrFlag) 0;
                            cur_op->val.deletel.a.typ = GREG;
                            cur_op->val.deletel.a.val.gr =
                                dventry.entry.memaccess.addr.val.gr;
                        }
                        else if (dventry.entry.memaccess.addr.typ == GSPEC){
                            // guest special address
                            cur_op->val.deletel.a.flag = (AddrFlag) 0;
                            cur_op->val.deletel.a.typ = GSPEC;
                            cur_op->val.deletel.a.val.gs =
                                dventry.entry.memaccess.addr.val.gs;
                        }
                        else if (dventry.entry.memaccess.addr.typ == MADDR){
                            // guest RAM
                            cur_op->val.deletel.a.flag = (AddrFlag) 0;
                            cur_op->val.deletel.a.typ = MADDR;
                            cur_op->val.deletel.a.val.ma =
                                dventry.entry.memaccess.addr.val.ma;
                        }
                        else {
                            assert(1==0);
                        }
                        break;

                    default:
                        // rest are unhandled for now
                        assert(1==0);
                }

                cur_op++;
            }

            buf->ptr = saved_buf_ptr;
        }

        else if ((dventry.entrytype == PADDRENTRY)
                && (dventry.entry.portaccess.op == PSTORE)) {
            /*** Fix up taint op buffer here ***/
            char *saved_buf_ptr = buf->ptr;
            TaintOp *cur_op = (TaintOp*) buf->ptr;

            int i;
            for (i = 0; i < op->val.insn_start.num_ops; i++){
                // For port accesses, we assume that data is only moved between
                // ports and guest registers.  If this assumption is wrong then
                // we will fail here.
                switch (cur_op->typ){
                    case COPYOP:
                        if (dventry.entry.portaccess.addr.typ == PADDR){
                            // guest register
                            cur_op->val.copy.b.flag = (AddrFlag) 0;
                            cur_op->val.copy.b.typ = PADDR;
                            cur_op->val.copy.b.val.pa =
                                dventry.entry.portaccess.addr.val.pa;
                        }
                        else {
                            assert(1==0);
                        }
                        break;
                    case DELETEOP:
                        if (dventry.entry.portaccess.addr.typ == PADDR){
                            // guest register
                            cur_op->val.deletel.a.flag = (AddrFlag) 0;
                            cur_op->val.deletel.a.typ = PADDR;
                            cur_op->val.deletel.a.val.pa =
                                dventry.entry.portaccess.addr.val.pa;
                        }
                        else {
                            assert(1==0);
                        }
                        break;
                    default:
                        // Ops for port store only consist of copy ops
                        assert(1==0);
                }

                cur_op++;
            }

            buf->ptr = saved_buf_ptr;
        }

        else {
            fprintf(stderr, "Error: unknown error in dynamic log\n");
            fprintf(stderr, "In: store\n");
            exit(1);
        }
    }

    else if (!strcmp(op->val.insn_start.name, "condbranch")){

        if (dventry.entrytype != BRANCHENTRY){
            fprintf(stderr, "Error: dynamic log doesn't align\n");
            fprintf(stderr, "In: branch\n");
            exit(1);
        }

        else if (dventry.entrytype == BRANCHENTRY) {

            /*** Fix up taint op buffer here ***/
            /*
             * The true branch is target[0] for brcond and br, and the
             * optional false branch is target[1], so that is how we log it
             */
            if (dventry.entry.branch.br == false){
                previous_branch = op->val.insn_start.cur_branch_bb;
                taken_branch = op->val.insn_start.branch_labels[0];
#ifdef TAINTDEBUG
                printf("Taken branch: %d\n", taken_branch);
#endif
            }
            else if (dventry.entry.branch.br == true) {
                previous_branch = op->val.insn_start.cur_branch_bb;
                taken_branch = op->val.insn_start.branch_labels[1];
#ifdef TAINTDEBUG
                printf("Taken branch: %d\n", taken_branch);
#endif
            }
            else {
                assert(1==0);
            }

            /*
             * Place to inspect taint on branch condition
             */
            //printf("Branch condition LLVM register: %%%d\n",
            //    op->val.insn_start.branch_cond_llvm_reg);

            next_step = BRANCH;
        }

        else {
            fprintf(stderr, "Error: unknown error in dynamic log\n");
            fprintf(stderr, "In: branch\n");
            exit(1);
        }
    }

    else if (!strcmp(op->val.insn_start.name, "switch")){

        if (dventry.entrytype != SWITCHENTRY){
            fprintf(stderr, "Error: dynamic log doesn't align\n");
            fprintf(stderr, "In: switch\n");
            exit(1);
        }

        else if (dventry.entrytype == SWITCHENTRY) {

            /*** Fix up taint op buffer here ***/

            int64_t switchCond = dventry.entry.switchstmt.cond;
            bool found = 0;

            unsigned len = op->val.insn_start.switch_len;
            unsigned i;
            for (i = 0; i < (len-1); i++){
                if (op->val.insn_start.switch_conds[i] == switchCond){
                    previous_branch = op->val.insn_start.cur_branch_bb;
                    taken_branch = op->val.insn_start.switch_labels[i];
                    found = 1;
                    break;
                }
            }

            // handle default case in switch
            if (!found){
                previous_branch = op->val.insn_start.cur_branch_bb;
                taken_branch = op->val.insn_start.switch_labels[len-1];
            }
#ifdef TAINTDEBUG
            printf("Switch cond: %ld\n", switchCond);
            printf("Taken branch: %d\n", taken_branch);
#endif
            next_step = SWITCHSTEP;
        }

        else {
            fprintf(stderr, "Error: unknown error in dynamic log\n");
            fprintf(stderr, "In: switch\n");
            exit(1);
        }
    }

    else if (!strcmp(op->val.insn_start.name, "select")){

        if (dventry.entrytype != SELECTENTRY){
            fprintf(stderr, "Error: dynamic log doesn't align\n");
            fprintf(stderr, "In: select\n");
            exit(1);
        }

        else if (dventry.entrytype == SELECTENTRY) {
            /*** Fix up taint op buffer here ***/

            TaintOp *cur_op = (TaintOp*) buf->ptr;
            char *saved_buf_ptr = buf->ptr;

            int i;
            for (i = 0; i < op->val.insn_start.num_ops; i++){
                // fill in src value
                cur_op->val.copy.a.flag = (AddrFlag) 0;
                cur_op->val.copy.a.typ = LADDR;
                if (dventry.entry.select.sel == false){
                    if (op->val.insn_start.branch_labels[0] == -1){
                        // select value was a constant, so we delete taint
                        // at dest
                        cur_op->typ = DELETEOP;
                        cur_op->val.deletel.a.val.la =
                            cur_op->val.copy.b.val.la;
                    }
                    else {
                        cur_op->val.copy.a.val.la =
                            op->val.insn_start.branch_labels[0];
                    }
                }
                else if (dventry.entry.select.sel == true){
                    if (op->val.insn_start.branch_labels[1] == -1){
                        // select value was a constant, so we delete taint
                        // at dest
                        cur_op->typ = DELETEOP;
                        cur_op->val.deletel.a.val.la =
                            cur_op->val.copy.b.val.la;
                    }
                    else {
                        cur_op->val.copy.a.val.la =
                            op->val.insn_start.branch_labels[1];
                    }
                }
                else {
                    assert(1==0);
                }

                cur_op++;
            }

            buf->ptr = saved_buf_ptr;
        }

        else {
            fprintf(stderr, "Error: unknown error in dynamic log\n");
            fprintf(stderr, "In: select\n");
            exit(1);
        }
    }
    else if (!strcmp(op->val.insn_start.name, "phi")){
        char *saved_buf_ptr = buf->ptr;
        TaintOp *cur_op = (TaintOp*) buf->ptr;

        /*** Fix up taint op buffer here ***/
        int phiSource = 0xDEADBEEF;
        int len = op->val.insn_start.phi_len;
        int i;
        for(i = 0; i < len; i++)
        {
            if(previous_branch == op->val.insn_start.phi_labels[i]) {
                //This is the source llvm register for the phi instruction
                //We need to copy taint from here to destination
                phiSource = op->val.insn_start.phi_vals[i];
                break;
            }
        }

        if (phiSource == (int)0xDEADBEEF){
            /* This means a match wasn't found between the previous basic block
             * executed and one of the predecessor basic blocks in the phi
             * instruction.  This should never happen.
             */
            assert(0);
        }

        //Skip copy operations if phiSource is a constant (-1)
        if(phiSource == -1) {
          //Move buffer pointer past copy operations
          cur_op += op->val.insn_start.num_ops;
          buf->ptr = (char*)cur_op;
        } else {
          //Patch up source for copy operations
          for (i = 0; i < op->val.insn_start.num_ops; i++){
              switch (cur_op->typ){
                  case COPYOP:
                    cur_op->val.copy.a.flag = (AddrFlag) 0;
                    cur_op->val.copy.a.typ = LADDR;
                    cur_op->val.copy.a.val.la = phiSource;
                    break;
                  default:
                    //Taint ops for phi only consist of copy ops
                    assert(1==0);
              }
              cur_op++;
          }
          buf->ptr = saved_buf_ptr;
        }
    }
    else if (!strcmp(op->val.insn_start.name, "memset")){
        if ((dventry.entrytype != ADDRENTRY)
                || (dventry.entry.memaccess.op != STORE)){
            fprintf(stderr, "Error: dynamic log doesn't align\n");
            fprintf(stderr, "In: memset\n");
            exit(1);
        }
        else if ((dventry.entrytype == ADDRENTRY)
                && (dventry.entry.memaccess.op == STORE)) {
            /*** Fix up taint op buffer here ***/
            char *saved_buf_ptr = buf->ptr;
            TaintOp *cur_op = (TaintOp*) buf->ptr;
            int i;
            for (i = 0; i < op->val.insn_start.num_ops; i++){
                switch (cur_op->typ){
                    case DELETEOP:
                        if (dventry.entry.memaccess.addr.flag == IRRELEVANT){
                            // do nothing for delete at address we aren't
                            // tracking
                            cur_op->val.deletel.a.flag = IRRELEVANT;
                        }
                        else if (dventry.entry.memaccess.addr.typ == GREG){
                            // guest register
                            cur_op->val.deletel.a.flag = (AddrFlag) 0;
                            cur_op->val.deletel.a.typ = GREG;
                            cur_op->val.deletel.a.val.gr =
                                dventry.entry.memaccess.addr.val.gr;
                        }
                        else if (dventry.entry.memaccess.addr.typ == GSPEC){
                            // guest special address
                            cur_op->val.deletel.a.flag = (AddrFlag) 0;
                            cur_op->val.deletel.a.typ = GSPEC;
                            cur_op->val.deletel.a.val.gs =
                                dventry.entry.memaccess.addr.val.gs;
                        }
                        else if (dventry.entry.memaccess.addr.typ == MADDR){
                            // guest RAM
                            cur_op->val.deletel.a.flag = (AddrFlag) 0;
                            cur_op->val.deletel.a.typ = MADDR;
                            cur_op->val.deletel.a.val.ma =
                                dventry.entry.memaccess.addr.val.ma;
                        }
                        else {
                            assert(1==0);
                        }
                        break;

                    default:
                        // taint ops for memset only consist of delete ops
                        assert(1==0);
                }
                cur_op++;
            }
            buf->ptr = saved_buf_ptr;
        }
    }
    else if (!strcmp(op->val.insn_start.name, "memcpy")){
        /*
         *MemCpy has two values in the dynamic log, the src and the dst.
         *The src is modeled as a LOAD and comes first in the log.
         *The dst is modeled as a STORE and comes second in the log.
         */

        DynValEntry dventry_src = dventry;
        DynValEntry dventry_dst;
        // Make sure there is still something to read in the buffer
        assert(((uintptr_t)(dynval_buf->ptr) - (uintptr_t)(dynval_buf->start))
            < dynval_buf->cur_size);

        read_dynval_buffer(dynval_buf, &dventry_dst);

        if (dventry_dst.entrytype == EXCEPTIONENTRY){
            printf("EXCEPTION FOUND IN DYNAMIC LOG\n");
            next_step = EXCEPT;
            return;
        }

        if ((dventry_src.entrytype != ADDRENTRY) ||
             (dventry_src.entry.memaccess.op != LOAD) ||
             (dventry_dst.entrytype != ADDRENTRY) ||
             (dventry_dst.entry.memaccess.op != STORE)) {
            fprintf(stderr, "Error: dynamic log doesn't align\n");
            fprintf(stderr, "In: memcpy\n");
            exit(1);
        } else {
            /*** Fix up taint op buffer here ***/
            char *saved_buf_ptr = buf->ptr;
            TaintOp *cur_op = (TaintOp*) buf->ptr;
            int i;
            for (i = 0; i < op->val.insn_start.num_ops; i++){
                switch (cur_op->typ){
                    case COPYOP:
                        if (dventry_src.entry.memaccess.addr.flag == IRRELEVANT){
                            // store to irrelevant part of CPU state
                            // delete taint at the destination
                            cur_op->val.copy.a.flag = IRRELEVANT;
                        }
                        else if (dventry_src.entry.memaccess.addr.typ == GREG){
                            // guest register
                            cur_op->val.copy.a.flag = (AddrFlag) 0;
                            cur_op->val.copy.a.typ = GREG;
                            cur_op->val.copy.a.val.gr =
                                dventry_src.entry.memaccess.addr.val.gr;
                        }
                        else if (dventry_src.entry.memaccess.addr.typ == GSPEC){
                            // guest special address
                            cur_op->val.copy.a.flag = (AddrFlag) 0;
                            cur_op->val.copy.a.typ = GSPEC;
                            cur_op->val.copy.a.val.gs =
                                dventry_src.entry.memaccess.addr.val.gs;
                        }
                        else if (dventry_src.entry.memaccess.addr.typ == MADDR){
                            // guest RAM
                            cur_op->val.copy.a.flag = (AddrFlag) 0;
                            cur_op->val.copy.a.typ = MADDR;
                            cur_op->val.copy.a.val.ma =
                                dventry_src.entry.memaccess.addr.val.ma;
                        }
                        else {
                            assert(1==0);
                        }

                        if (dventry_dst.entry.memaccess.addr.flag == IRRELEVANT){
                            // store to irrelevant part of CPU state
                            // delete taint at the destination
                            cur_op->val.copy.b.flag = IRRELEVANT;
                        }
                        else if (dventry_dst.entry.memaccess.addr.typ == GREG){
                            // guest register
                            cur_op->val.copy.b.flag = (AddrFlag) 0;
                            cur_op->val.copy.b.typ = GREG;
                            cur_op->val.copy.b.val.gr =
                                dventry_dst.entry.memaccess.addr.val.gr;
                        }
                        else if (dventry_dst.entry.memaccess.addr.typ == GSPEC){
                            // guest special address
                            cur_op->val.copy.b.flag = (AddrFlag) 0;
                            cur_op->val.copy.b.typ = GSPEC;
                            cur_op->val.copy.b.val.gs =
                                dventry_dst.entry.memaccess.addr.val.gs;
                        }
                        else if (dventry_dst.entry.memaccess.addr.typ == MADDR){
                            // guest RAM
                            cur_op->val.copy.b.flag = (AddrFlag) 0;
                            cur_op->val.copy.b.typ = MADDR;
                            cur_op->val.copy.b.val.ma =
                                dventry_dst.entry.memaccess.addr.val.ma;
                        }
                        else {
                            assert(1==0);
                        }

                        break;
                    default:
                        // taint ops for memcpy only consist of copy ops
                        assert(1==0);
                }
                cur_op++;
            }
            buf->ptr = saved_buf_ptr;
        }
    }
}
//
//SB_INLINE void tob_process(TaintOpBuffer *buf, Shad *shad,
//        DynValBuffer *dynval_buf) {
void top_process(TaintOp *op, Shad *shad, DynValBuffer *dynval_buf) {

#ifdef TAINTDEBUG
    printf("op %d ", i);
    tob_op_print(shad, op);
#endif
    switch (op->typ) {
        case LABELOP:
            {
                tp_label(shad, &(op->val.label.a), op->val.label.l);
                break;
            }

        case DELETEOP:
            {
                /* if it's a delete of an address we aren't tracking,
                 * do nothing
                 */
                if (op->val.copy.a.flag == IRRELEVANT){
                    break;
                }
#ifdef TAINTDEBUG
                if (tp_query(shad, &(op->val.deletel.a))) {
                    printf ("  [removes taint]\n");
                }
#endif
                tp_delete(shad, &(op->val.deletel.a));
                break;
            }

        case COPYOP:
            {
                /* if source is address we aren't tracking, then delete the
                 * taint at dest
                 */
                if (op->val.copy.a.flag == IRRELEVANT){
#ifdef TAINTDEBUG
                    uint8_t foo = 0;
                    if (tp_query(shad, &(op->val.copy.b))){
                        printf ("  [dest was tainted]"); foo = 1;
                    }
                    if (foo) printf("\n");
#endif
                    tp_delete(shad, &(op->val.copy.b));
                    break;
                }

                /* if it's a copy to an address we aren't tracking, do
                 * nothing
                 */
                if (op->val.copy.b.flag == IRRELEVANT){
                    break;
                }

#ifdef TAINTDEBUG
                uint8_t foo = 0;
                if (tp_query(shad, &(op->val.copy.a))) {
                    printf ("  [src is tainted]"); foo = 1;
                }
                if (tp_query(shad, &(op->val.copy.b))) {
                    printf ("  [dest was tainted]"); foo = 1;
                }
                if (foo) printf("\n");
#endif
                tp_copy(shad, &(op->val.copy.a), &(op->val.copy.b));
                break;
            }

        case LDCALLBACKOP:
            {
                if (ppp_on_load_num_cb > 0) {
                    // semantically right after ld has happened.
                    uint64_t ld_addr = op->val.ldcallback.a.val.ma + op->val.ldcallback.a.off;
                    PPP_RUN_CB(on_load, shad->pc, ld_addr);
                }
                break;
            }


        case STCALLBACKOP:
            {
                if (ppp_on_store_num_cb > 0) {  
                    // semantically right after st has happened.
                    uint64_t st_addr = op->val.stcallback.a.val.ma + op->val.stcallback.a.off;
                    PPP_RUN_CB(on_store, shad->pc, st_addr);
                }
                break;
            }


        case BULKCOPYOP:
            // TRL this is used by hd taint.  idea is to
            // specify a src and dest and a number of bytes to copy
            {
                uint32_t i;
                Addr a,b;
                a = op->val.bulkcopy.a;
                b = op->val.bulkcopy.b;
                for (i=0; i<op->val.bulkcopy.l; i++) {
#ifdef TAINTDEBUG
                    uint8_t foo = 0;
                    if (tp_query(shad, &(op->val.bulkcopy.a))) {
                        printf ("  [src is tainted]"); foo = 1;
                    }
                    if (tp_query(shad, &(op->val.bulkcopy.b))) {
                        printf ("  [dest was tainted]"); foo = 1;
                    }
                    if (foo) printf("\n");
#endif
                    tp_copy(shad, &a, &b);
                    addr_inc(&a);
                    addr_inc(&b);
                    //                        tp_copy(shad, addr_add(op->val.bulkcopy.a, i),
                    //				addr_add(op->val.bulkcopy.b, i));
                }

                break;
            }

        case COMPUTEOP:
            {
                /* if it's a compute to an address we aren't tracking, do
                 * nothing
                 */
                if (op->val.compute.c.flag == IRRELEVANT){
                    break;
                }

                /* in tainted pointer mode, if for some reason the pointer
                 * is tainted but it points to a guest register, do nothing
                 */

                if (tainted_pointer) {
                    if (op->val.compute.c.typ == GREG){
                        break;
                    } else if (op->val.compute.c.typ == GSPEC){
                        break;
                    }
                } // tainted_pointer is on

#ifdef TAINTDEBUG
                uint8_t foo = 0;
                if (tp_query(shad, &(op->val.compute.a))) {
                    printf ("  [src1 was tainted]"); foo = 1;
                }
                if (tp_query(shad, &(op->val.compute.b))) {
                    printf ("  [src2 was tainted]"); foo = 1;
                }
                if (tp_query(shad, &(op->val.compute.c))) {
                    printf ("  [dest was tainted]"); foo = 1;
                }
                if (foo) printf("\n");
#endif
                tp_compute(shad, &(op->val.compute.a), &(op->val.compute.b),
                        &(op->val.compute.c));
                break;
            }


        case PCOP:
            {
                if (tainted_instructions && shad->taint_state_changed) {
                    // add last pc to set of pcs that changed taint state
                    shad->tpc[shad->asid].insert(shad->pc);
                }

                // set taint processor's pc to correct value for
                // current instruction
                shad->pc = op->val.pc;
                //		    printf ("shad->pc = 0x%lx\n", shad->pc);

                if (tainted_instructions) {
                    // clear this so that we will know if taint state changed for this instruction
                    shad->taint_state_changed = 0;
                }

                break;
            }

        case INSNSTARTOP:
            {
                process_insn_start_op(op, buf, dynval_buf);
                if (next_step == EXCEPT){
                    return;
                }
                break;
            }

        case CALLOP:
            {
                shad->current_frame = shad->current_frame + 1;
                execute_taint_ops(op->val.call.ttb, shad, dynval_buf);
                break;
            }

        case RETOP:
            {
                if (shad->current_frame > 0){
                    shad->current_frame = shad->current_frame - 1;
                }
                else if ((int)shad->current_frame < 0){
                    assert(1==0);
                }
                break;
            }

        case QUERYOP:
            {
                bufplot(NULL, shad, &(op->val.query.a), op->val.query.l);
                break;
            }

        default:
            assert (1==0);
    }
}
