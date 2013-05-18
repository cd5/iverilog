/*
 * Copyright (c) 2001-2013 Stephen Williams (steve@icarus.com)
 *
 *    This source code is free software; you can redistribute it
 *    and/or modify it in source code form under the terms of the GNU
 *    General Public License as published by the Free Software
 *    Foundation; either version 2 of the License, or (at your option)
 *    any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with this program; if not, write to the Free Software
 *    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

# include  "config.h"
# include  "vthread.h"
# include  "codes.h"
# include  "schedule.h"
# include  "ufunc.h"
# include  "event.h"
# include  "vpi_priv.h"
# include  "vvp_net_sig.h"
# include  "vvp_cobject.h"
# include  "vvp_darray.h"
# include  "class_type.h"
#ifdef CHECK_WITH_VALGRIND
# include  "vvp_cleanup.h"
#endif
# include  <set>
# include  <typeinfo>
# include  <vector>
# include  <cstdlib>
# include  <climits>
# include  <cstring>
# include  <cmath>
# include  <cassert>

# include  <iostream>
# include  <cstdio>

using namespace std;

/* This is the size of an unsigned long in bits. This is just a
   convenience macro. */
# define CPU_WORD_BITS (8*sizeof(unsigned long))
# define TOP_BIT (1UL << (CPU_WORD_BITS-1))

/*
 * This vthread_s structure describes all there is to know about a
 * thread, including its program counter, all the private bits it
 * holds, and its place in other lists.
 *
 *
 * ** Notes On The Interactions of %fork/%join/%end:
 *
 * The %fork instruction creates a new thread and pushes that into a
 * set of children for the thread. This new thread, then, becomes a
 * child of the current thread, and the current thread a parent of the
 * new thread. Any child can be reaped by a %join.
 *
 * Children placed into an automatic scope are given special
 * treatment, which is required to make function/tasks calls that they
 * represent work correctly. These automatic children are copied into
 * an automatic_children set to mark them for this handling. %join
 * operations will guarantee that automatic threads are joined first,
 * before any non-automatic threads.
 *
 * It is a programming error for a thread that created threads to not
 * %join (or %join/detach) as many as it created before it %ends. The
 * children set will get messed up otherwise.
 *
 * the i_am_joining flag is a clue to children that the parent is
 * blocked in a %join and may need to be scheduled. The %end
 * instruction will check this flag in the parent to see if it should
 * notify the parent that something is interesting.
 *
 * The i_have_ended flag, on the other hand, is used by threads to
 * tell their parents that they are already dead. A thread that
 * executes %end will set its own i_have_ended flag and let its parent
 * reap it when the parent does the %join. If a thread has its
 * schedule_parent_on_end flag set already when it %ends, then it
 * reaps itself and simply schedules its parent. If a child has its
 * i_have_ended flag set when a thread executes %join, then it is free
 * to reap the child immediately.
 */

struct vthread_s {
      vthread_s();

	/* This is the program counter. */
      vvp_code_t pc;
	/* These hold the private thread bits. */
      vvp_vector4_t bits4;

	/* These are the word registers. */
      union {
	    int64_t  w_int;
	    uint64_t w_uint;
      } words[16];

    private:
      vector<double> stack_real_;
    public:
      inline double pop_real(void)
      {
	    assert(! stack_real_.empty());
	    double val = stack_real_.back();
	    stack_real_.pop_back();
	    return val;
      }
      inline void push_real(double val)
      {
	    stack_real_.push_back(val);
      }
      inline double peek_real(unsigned depth)
      {
	    assert(depth < stack_real_.size());
	    unsigned use_index = stack_real_.size()-1-depth;
	    return stack_real_[use_index];
      }
      inline void pop_real(unsigned cnt)
      {
	    while (cnt > 0) {
		  stack_real_.pop_back();
		  cnt -= 1;
	    }
      }

	/* Strings are operated on using a forth-like operator
	   set. Items at the top of the stack (back()) are the objects
	   operated on except for special cases. New objects are
	   pushed onto the top (back()) and pulled from the top
	   (back()) only. */
    private:
      vector<string> stack_str_;
    public:
      inline string pop_str(void)
      {
	    assert(! stack_str_.empty());
	    string val = stack_str_.back();
	    stack_str_.pop_back();
	    return val;
      }
      inline void push_str(const string&val)
      {
	    stack_str_.push_back(val);
      }
      inline string&peek_str(unsigned depth)
      {
	    assert(depth<stack_str_.size());
	    unsigned use_index = stack_str_.size()-1-depth;
	    return stack_str_[use_index];
      }
      inline void pop_str(unsigned cnt)
      {
	    while (cnt > 0) {
		  stack_str_.pop_back();
		  cnt -= 1;
	    }
      }

	/* Objects are also operated on in a stack. */
    private:
      enum { STACK_OBJ_MAX_SIZE = 32 };
      vvp_object_t stack_obj_[STACK_OBJ_MAX_SIZE];
      unsigned stack_obj_size_;
    public:
      inline vvp_object_t& peek_object(void)
      {
	    assert(stack_obj_size_ > 0);
	    return stack_obj_[stack_obj_size_-1];
      }
      inline void pop_object(vvp_object_t&obj)
      {
	    assert(stack_obj_size_ > 0);
	    stack_obj_size_ -= 1;
	    obj = stack_obj_[stack_obj_size_];
	    stack_obj_[stack_obj_size_].reset(0);
      }
      inline void pop_object(unsigned cnt)
      {
	    assert(cnt <= stack_obj_size_);
	    for (size_t idx = stack_obj_size_-cnt ; idx < stack_obj_size_ ; idx += 1)
		  stack_obj_[idx].reset(0);
	    stack_obj_size_ -= cnt;
      }
      inline void push_object(const vvp_object_t&obj)
      {
	    assert(stack_obj_size_ < STACK_OBJ_MAX_SIZE);
	    stack_obj_[stack_obj_size_] = obj;
	    stack_obj_size_ += 1;
      }

	/* My parent sets this when it wants me to wake it up. */
      unsigned i_am_joining      :1;
      unsigned i_have_ended      :1;
      unsigned waiting_for_event :1;
      unsigned is_scheduled      :1;
      unsigned delay_delete      :1;
	/* This points to the children of the thread. */
      set<struct vthread_s*>children;
	/* No more than 1 of the children are automatic. */
      set<vthread_s*>automatic_children;
	/* This points to my parent, if I have one. */
      struct vthread_s*parent;
	/* This points to the containing scope. */
      struct __vpiScope*parent_scope;
	/* This is used for keeping wait queues. */
      struct vthread_s*wait_next;
	/* These are used to access automatically allocated items. */
      vvp_context_t wt_context, rd_context;
	/* These are used to pass non-blocking event control information. */
      vvp_net_t*event;
      uint64_t ecount;

      inline void cleanup()
      {
	    bits4 = vvp_vector4_t();
	    assert(stack_real_.empty());
	    assert(stack_str_.empty());
      }
};

inline vthread_s::vthread_s()
{
      stack_obj_size_ = 0;
}

static bool test_joinable(vthread_t thr, vthread_t child);
static void do_join(vthread_t thr, vthread_t child);

struct __vpiScope* vthread_scope(struct vthread_s*thr)
{
      return thr->parent_scope;
}

struct vthread_s*running_thread = 0;

// this table maps the thread special index bit addresses to
// vvp_bit4_t bit values.
static vvp_bit4_t thr_index_to_bit4[4] = { BIT4_0, BIT4_1, BIT4_X, BIT4_Z };

static inline void thr_check_addr(struct vthread_s*thr, unsigned addr)
{
      if (thr->bits4.size() <= addr)
	    thr->bits4.resize(addr+1);
}

static inline vvp_bit4_t thr_get_bit(struct vthread_s*thr, unsigned addr)
{
      assert(addr < thr->bits4.size());
      return thr->bits4.value(addr);
}

static inline void thr_put_bit(struct vthread_s*thr,
			       unsigned addr, vvp_bit4_t val)
{
      thr_check_addr(thr, addr);
      thr->bits4.set_bit(addr, val);
}

// REMOVE ME
static inline void thr_clr_bit_(struct vthread_s*thr, unsigned addr)
{
      thr->bits4.set_bit(addr, BIT4_0);
}

vvp_bit4_t vthread_get_bit(struct vthread_s*thr, unsigned addr)
{
      if (vpi_mode_flag == VPI_MODE_COMPILETF) return BIT4_X;
      else return thr_get_bit(thr, addr);
}

void vthread_put_bit(struct vthread_s*thr, unsigned addr, vvp_bit4_t bit)
{
      thr_put_bit(thr, addr, bit);
}

void vthread_push_real(struct vthread_s*thr, double val)
{
      thr->push_real(val);
}

void vthread_pop_real(struct vthread_s*thr, unsigned depth)
{
      thr->pop_real(depth);
}

void vthread_pop_str(struct vthread_s*thr, unsigned depth)
{
      thr->pop_str(depth);
}

const string&vthread_get_str_stack(struct vthread_s*thr, unsigned depth)
{
      return thr->peek_str(depth);
}

double vthread_get_real_stack(struct vthread_s*thr, unsigned depth)
{
      return thr->peek_real(depth);
}

template <class T> T coerce_to_width(const T&that, unsigned width)
{
      if (that.size() == width)
	    return that;

      assert(that.size() > width);
      T res (width);
      for (unsigned idx = 0 ;  idx < width ;  idx += 1)
	    res.set_bit(idx, that.value(idx));

      return res;
}

static unsigned long* vector_to_array(struct vthread_s*thr,
				      unsigned addr, unsigned wid)
{
      if (addr == 0) {
	    unsigned awid = (wid + CPU_WORD_BITS - 1) / (CPU_WORD_BITS);
	    unsigned long*val = new unsigned long[awid];
	    for (unsigned idx = 0 ;  idx < awid ;  idx += 1)
		  val[idx] = 0;
	    return val;
      }
      if (addr == 1) {
	    unsigned awid = (wid + CPU_WORD_BITS - 1) / (CPU_WORD_BITS);
	    unsigned long*val = new unsigned long[awid];
	    for (unsigned idx = 0 ;  idx < awid ;  idx += 1)
		  val[idx] = -1UL;

	    wid -= (awid-1) * CPU_WORD_BITS;
	    if (wid < CPU_WORD_BITS)
		  val[awid-1] &= (-1UL) >> (CPU_WORD_BITS-wid);

	    return val;
      }

      if (addr < 4)
	    return 0;

      return thr->bits4.subarray(addr, wid);
}

/*
 * This function gets from the thread a vector of bits starting from
 * the addressed location and for the specified width.
 */
static vvp_vector4_t vthread_bits_to_vector(struct vthread_s*thr,
					    unsigned bit, unsigned wid)
{
	/* Make a vector of the desired width. */

      if (bit >= 4) {
	    return vvp_vector4_t(thr->bits4, bit, wid);

      } else {
	    return vvp_vector4_t(wid, thr_index_to_bit4[bit]);
      }
}

/*
 * Some of the instructions do wide addition to arrays of long. They
 * use this add_with_carry function to help.
 */
static inline unsigned long add_with_carry(unsigned long a, unsigned long b,
					   unsigned long&carry)
{
      unsigned long tmp = b + carry;
      unsigned long sum = a + tmp;
      carry = 0;
      if (tmp < b)
	    carry = 1;
      if (sum < tmp)
	    carry = 1;
      if (sum < a)
	    carry = 1;
      return sum;
}

static unsigned long multiply_with_carry(unsigned long a, unsigned long b,
					 unsigned long&carry)
{
      const unsigned long mask = (1UL << (CPU_WORD_BITS/2)) - 1;
      unsigned long a0 = a & mask;
      unsigned long a1 = (a >> (CPU_WORD_BITS/2)) & mask;
      unsigned long b0 = b & mask;
      unsigned long b1 = (b >> (CPU_WORD_BITS/2)) & mask;

      unsigned long tmp = a0 * b0;

      unsigned long r00 = tmp & mask;
      unsigned long c00 = (tmp >> (CPU_WORD_BITS/2)) & mask;

      tmp = a0 * b1;

      unsigned long r01 = tmp & mask;
      unsigned long c01 = (tmp >> (CPU_WORD_BITS/2)) & mask;

      tmp = a1 * b0;

      unsigned long r10 = tmp & mask;
      unsigned long c10 = (tmp >> (CPU_WORD_BITS/2)) & mask;

      tmp = a1 * b1;

      unsigned long r11 = tmp & mask;
      unsigned long c11 = (tmp >> (CPU_WORD_BITS/2)) & mask;

      unsigned long r1 = c00 + r01 + r10;
      unsigned long r2 = (r1 >> (CPU_WORD_BITS/2)) & mask;
      r1 &= mask;
      r2 += c01 + c10 + r11;
      unsigned long r3 = (r2 >> (CPU_WORD_BITS/2)) & mask;
      r2 &= mask;
      r3 += c11;
      r3 &= mask;

      carry = (r3 << (CPU_WORD_BITS/2)) + r2;
      return (r1 << (CPU_WORD_BITS/2)) + r00;
}

static void multiply_array_imm(unsigned long*res, unsigned long*val,
			       unsigned words, unsigned long imm)
{
      for (unsigned idx = 0 ; idx < words ; idx += 1)
	    res[idx] = 0;

      for (unsigned mul_idx = 0 ; mul_idx < words ; mul_idx += 1) {
	    unsigned long sum;
	    unsigned long tmp = multiply_with_carry(val[mul_idx], imm, sum);

	    unsigned long carry = 0;
	    res[mul_idx] = add_with_carry(res[mul_idx], tmp, carry);
	    for (unsigned add_idx = mul_idx+1 ; add_idx < words ; add_idx += 1) {
		  res[add_idx] = add_with_carry(res[add_idx], sum, carry);
		  sum = 0;
	    }
      }
}

/*
 * Allocate a context for use by a child thread. By preference, use
 * the last freed context. If none available, create a new one. Add
 * it to the list of live contexts in that scope.
 */
static vvp_context_t vthread_alloc_context(struct __vpiScope*scope)
{
      assert(scope->is_automatic);

      vvp_context_t context = scope->free_contexts;
      if (context) {
            scope->free_contexts = vvp_get_next_context(context);
            for (unsigned idx = 0 ; idx < scope->nitem ; idx += 1) {
                  scope->item[idx]->reset_instance(context);
            }
      } else {
            context = vvp_allocate_context(scope->nitem);
            for (unsigned idx = 0 ; idx < scope->nitem ; idx += 1) {
                  scope->item[idx]->alloc_instance(context);
            }
      }

      vvp_set_next_context(context, scope->live_contexts);
      scope->live_contexts = context;

      return context;
}

/*
 * Free a context previously allocated to a child thread by pushing it
 * onto the freed context stack. Remove it from the list of live contexts
 * in that scope.
 */
static void vthread_free_context(vvp_context_t context, struct __vpiScope*scope)
{
      assert(scope->is_automatic);
      assert(context);

      if (context == scope->live_contexts) {
            scope->live_contexts = vvp_get_next_context(context);
      } else {
            vvp_context_t tmp = scope->live_contexts;
            while (context != vvp_get_next_context(tmp)) {
                  assert(tmp);
                  tmp = vvp_get_next_context(tmp);
            }
            vvp_set_next_context(tmp, vvp_get_next_context(context));
      }

      vvp_set_next_context(context, scope->free_contexts);
      scope->free_contexts = context;
}

#ifdef CHECK_WITH_VALGRIND
void contexts_delete(struct __vpiScope*scope)
{
      vvp_context_t context = scope->free_contexts;

      while (context) {
	    scope->free_contexts = vvp_get_next_context(context);
	    for (unsigned idx = 0; idx < scope->nitem; idx += 1) {
		  scope->item[idx]->free_instance(context);
	    }
	    free(context);
	    context = scope->free_contexts;
      }
      free(scope->item);
}
#endif

/*
 * Create a new thread with the given start address.
 */
vthread_t vthread_new(vvp_code_t pc, struct __vpiScope*scope)
{
      vthread_t thr = new struct vthread_s;
      thr->pc     = pc;
      thr->bits4  = vvp_vector4_t(32);
      thr->parent = 0;
      thr->parent_scope = scope;
      thr->wait_next = 0;
      thr->wt_context = 0;
      thr->rd_context = 0;

      thr->i_am_joining = 0;
      thr->is_scheduled = 0;
      thr->i_have_ended = 0;
      thr->delay_delete = 0;
      thr->waiting_for_event = 0;
      thr->event  = 0;
      thr->ecount = 0;

      thr_put_bit(thr, 0, BIT4_0);
      thr_put_bit(thr, 1, BIT4_1);
      thr_put_bit(thr, 2, BIT4_X);
      thr_put_bit(thr, 3, BIT4_Z);

      scope->threads .insert(thr);
      return thr;
}

#ifdef CHECK_WITH_VALGRIND
#if 0
/*
 * These are not currently correct. If you use them you will get
 * double delete messages. There is still a leak related to a
 * waiting event that needs to be investigated.
 */

static void wait_next_delete(vthread_t base)
{
      while (base) {
	    vthread_t tmp = base->wait_next;
	    delete base;
	    base = tmp;
	    if (base->waiting_for_event == 0) break;
      }
}

static void child_delete(vthread_t base)
{
      while (base) {
	    vthread_t tmp = base->child;
	    delete base;
	    base = tmp;
      }
}
#endif

void vthreads_delete(struct __vpiScope*scope)
{
      for (std::set<vthread_t>::iterator cur = scope->threads.begin()
		 ; cur != scope->threads.end() ; ++ cur ) {
	    delete *cur;
      }
      scope->threads.clear();
}
#endif

/*
 * Reaping pulls the thread out of the stack of threads. If I have a
 * child, then hand it over to my parent.
 */
static void vthread_reap(vthread_t thr)
{
      if (! thr->children.empty()) {
	    for (set<vthread_t>::iterator cur = thr->children.begin()
		       ; cur != thr->children.end() ; ++cur) {
		  vthread_t curp = *cur;
		  assert(curp->parent == thr);
		  curp->parent = thr->parent;
	    }
      }
      if (thr->parent) {
	      //assert(thr->parent->child == thr);
	    thr->parent->children.erase(thr);
      }

      thr->parent = 0;

	// Remove myself from the containing scope.
      thr->parent_scope->threads.erase(thr);

      thr->pc = codespace_null();

	/* If this thread is not scheduled, then is it safe to delete
	   it now. Otherwise, let the schedule event (which will
	   execute the thread at of_ZOMBIE) delete the object. */
      if ((thr->is_scheduled == 0) && (thr->waiting_for_event == 0)) {
	    assert(thr->children.empty());
	    assert(thr->wait_next == 0);
	    if (thr->delay_delete)
		  schedule_del_thr(thr);
	    else
		  vthread_delete(thr);
      }
}

void vthread_delete(vthread_t thr)
{
      thr->cleanup();
      delete thr;
}

void vthread_mark_scheduled(vthread_t thr)
{
      while (thr != 0) {
	    assert(thr->is_scheduled == 0);
	    thr->is_scheduled = 1;
	    thr = thr->wait_next;
      }
}

void vthread_delay_delete()
{
      if (running_thread)
	    running_thread->delay_delete = 1;
}

/*
 * This function runs each thread by fetching an instruction,
 * incrementing the PC, and executing the instruction. The thread may
 * be the head of a list, so each thread is run so far as possible.
 */
void vthread_run(vthread_t thr)
{
      while (thr != 0) {
	    vthread_t tmp = thr->wait_next;
	    thr->wait_next = 0;

	    assert(thr->is_scheduled);
	    thr->is_scheduled = 0;

            running_thread = thr;

	    for (;;) {
		  vvp_code_t cp = thr->pc;
		  thr->pc += 1;

		    /* Run the opcode implementation. If the execution of
		       the opcode returns false, then the thread is meant to
		       be paused, so break out of the loop. */
		  bool rc = (cp->opcode)(thr, cp);
		  if (rc == false)
			break;
	    }

	    thr = tmp;
      }
      running_thread = 0;
}

/*
 * The CHUNK_LINK instruction is a special next pointer for linking
 * chunks of code space. It's like a simplified %jmp.
 */
bool of_CHUNK_LINK(vthread_t thr, vvp_code_t code)
{
      assert(code->cptr);
      thr->pc = code->cptr;
      return true;
}

/*
 * This is called by an event functor to wake up all the threads on
 * its list. I in fact created that list in the %wait instruction, and
 * I also am certain that the waiting_for_event flag is set.
 */
void vthread_schedule_list(vthread_t thr)
{
      for (vthread_t cur = thr ;  cur ;  cur = cur->wait_next) {
	    assert(cur->waiting_for_event);
	    cur->waiting_for_event = 0;
      }

      schedule_vthread(thr, 0);
}

vvp_context_t vthread_get_wt_context()
{
      if (running_thread)
            return running_thread->wt_context;
      else
            return 0;
}

vvp_context_t vthread_get_rd_context()
{
      if (running_thread)
            return running_thread->rd_context;
      else
            return 0;
}

vvp_context_item_t vthread_get_wt_context_item(unsigned context_idx)
{
      assert(running_thread && running_thread->wt_context);
      return vvp_get_context_item(running_thread->wt_context, context_idx);
}

vvp_context_item_t vthread_get_rd_context_item(unsigned context_idx)
{
      assert(running_thread && running_thread->rd_context);
      return vvp_get_context_item(running_thread->rd_context, context_idx);
}

bool of_ABS_WR(vthread_t thr, vvp_code_t)
{
      thr->push_real( fabs(thr->pop_real()) );
      return true;
}

bool of_ALLOC(vthread_t thr, vvp_code_t cp)
{
        /* Allocate a context. */
      vvp_context_t child_context = vthread_alloc_context(cp->scope);

        /* Push the allocated context onto the write context stack. */
      vvp_set_stacked_context(child_context, thr->wt_context);
      thr->wt_context = child_context;

      return true;
}

static bool of_AND_wide(vthread_t thr, vvp_code_t cp)
{
      unsigned idx1 = cp->bit_idx[0];
      unsigned idx2 = cp->bit_idx[1];
      unsigned wid = cp->number;

      vvp_vector4_t val = vthread_bits_to_vector(thr, idx1, wid);
      val &= vthread_bits_to_vector(thr, idx2, wid);
      thr->bits4.set_vec(idx1, val);

      return true;
}

static bool of_AND_narrow(vthread_t thr, vvp_code_t cp)
{
      unsigned idx1 = cp->bit_idx[0];
      unsigned idx2 = cp->bit_idx[1];
      unsigned wid = cp->number;

      for (unsigned idx = 0 ; idx < wid ; idx += 1) {
	    vvp_bit4_t lb = thr_get_bit(thr, idx1);
	    vvp_bit4_t rb = thr_get_bit(thr, idx2);
	    thr_put_bit(thr, idx1, lb&rb);
	    idx1 += 1;
	    if (idx2 >= 4)
		  idx2 += 1;
      }

      return true;
}

bool of_AND(vthread_t thr, vvp_code_t cp)
{
      assert(cp->bit_idx[0] >= 4);

      if (cp->number <= 4)
	    cp->opcode = &of_AND_narrow;
      else
	    cp->opcode = &of_AND_wide;

      return cp->opcode(thr, cp);
}


bool of_ANDI(vthread_t thr, vvp_code_t cp)
{
      unsigned idx1 = cp->bit_idx[0];
      unsigned long imm = cp->bit_idx[1];
      unsigned wid = cp->number;

      assert(idx1 >= 4);

      vvp_vector4_t val = vthread_bits_to_vector(thr, idx1, wid);
      vvp_vector4_t imv (wid, BIT4_0);

      unsigned trans = wid;
      if (trans > CPU_WORD_BITS)
	    trans = CPU_WORD_BITS;
      imv.setarray(0, trans, &imm);

      val &= imv;

      thr->bits4.set_vec(idx1, val);
      return true;
}

bool of_ADD(vthread_t thr, vvp_code_t cp)
{
      assert(cp->bit_idx[0] >= 4);

      unsigned long*lva = vector_to_array(thr, cp->bit_idx[0], cp->number);
      unsigned long*lvb = vector_to_array(thr, cp->bit_idx[1], cp->number);
      if (lva == 0 || lvb == 0)
	    goto x_out;

      unsigned long carry;
      carry = 0;
      for (unsigned idx = 0 ;  (idx*CPU_WORD_BITS) < cp->number ;  idx += 1)
	    lva[idx] = add_with_carry(lva[idx], lvb[idx], carry);

	/* We know from the vector_to_array that the address is valid
	   in the thr->bitr4 vector, so just do the set bit. */

      thr->bits4.setarray(cp->bit_idx[0], cp->number, lva);

      delete[]lva;
      delete[]lvb;

      return true;

 x_out:
      delete[]lva;
      delete[]lvb;

      vvp_vector4_t tmp(cp->number, BIT4_X);
      thr->bits4.set_vec(cp->bit_idx[0], tmp);

      return true;
}

bool of_ADD_WR(vthread_t thr, vvp_code_t)
{
      double r = thr->pop_real();
      double l = thr->pop_real();
      thr->push_real(l + r);
      return true;
}

/*
 * This is %addi, add-immediate. The first value is a vector, the
 * second value is the immediate value in the bin_idx[1] position. The
 * immediate value can be up to 16 bits, which are then padded to the
 * width of the vector with zero.
 */
bool of_ADDI(vthread_t thr, vvp_code_t cp)
{
	// Collect arguments
      unsigned bit_addr       = cp->bit_idx[0];
      unsigned long imm_value = cp->bit_idx[1];
      unsigned bit_width      = cp->number;

      assert(bit_addr >= 4);

      unsigned word_count = (bit_width+CPU_WORD_BITS-1)/CPU_WORD_BITS;

      unsigned long*lva = vector_to_array(thr, bit_addr, bit_width);
      if (lva == 0)
	    goto x_out;


      unsigned long carry;
      carry = 0;
      for (unsigned idx = 0 ;  idx < word_count ;  idx += 1) {
	    lva[idx] = add_with_carry(lva[idx], imm_value, carry);
	    imm_value = 0;
      }

	/* We know from the vector_to_array that the address is valid
	   in the thr->bitr4 vector, so just do the set bit. */

      thr->bits4.setarray(bit_addr, bit_width, lva);

      delete[]lva;

      return true;

 x_out:
      delete[]lva;

      vvp_vector4_t tmp (bit_width, BIT4_X);
      thr->bits4.set_vec(bit_addr, tmp);

      return true;
}

/* %assign/ar <array>, <delay>
 * Generate an assignment event to a real array. Index register 3
 * contains the canonical address of the word in the memory. <delay>
 * is the delay in simulation time. <bit> is the index register
 * containing the real value.
 */
bool of_ASSIGN_AR(vthread_t thr, vvp_code_t cp)
{
      long adr = thr->words[3].w_int;
      unsigned delay = cp->bit_idx[0];
      double value = thr->pop_real();

      if (adr >= 0) {
	    schedule_assign_array_word(cp->array, adr, value, delay);
      }

      return true;
}

/* %assign/ar/d <array>, <delay_idx>
 * Generate an assignment event to a real array. Index register 3
 * contains the canonical address of the word in the memory.
 * <delay_idx> is the integer register that contains the delay value.
 */
bool of_ASSIGN_ARD(vthread_t thr, vvp_code_t cp)
{
      long adr = thr->words[3].w_int;
      vvp_time64_t delay = thr->words[cp->bit_idx[0]].w_uint;
      double value = thr->pop_real();

      if (adr >= 0) {
	    schedule_assign_array_word(cp->array, adr, value, delay);
      }

      return true;
}

/* %assign/ar/e <array>
 * Generate an assignment event to a real array. Index register 3
 * contains the canonical address of the word in the memory. <bit>
 * is the index register containing the real value. The event
 * information is contained in the thread event control registers
 * and is set with %evctl.
 */
bool of_ASSIGN_ARE(vthread_t thr, vvp_code_t cp)
{
      long adr = thr->words[3].w_int;
      double value = thr->pop_real();

      if (adr >= 0) {
	    if (thr->ecount == 0) {
		  schedule_assign_array_word(cp->array, adr, value, 0);
	    } else {
		  schedule_evctl(cp->array, adr, value, thr->event,
		                 thr->ecount);
	    }
      }

      return true;
}

/* %assign/av <array>, <delay>, <bit>
 * This generates an assignment event to an array. Index register 0
 * contains the width of the vector (and the word) and index register
 * 3 contains the canonical address of the word in memory.
 */
bool of_ASSIGN_AV(vthread_t thr, vvp_code_t cp)
{
      unsigned wid = thr->words[0].w_int;
      long off = thr->words[1].w_int;
      long adr = thr->words[3].w_int;
      unsigned delay = cp->bit_idx[0];
      unsigned bit = cp->bit_idx[1];

      if (adr < 0) return true;

      long vwidth = get_array_word_size(cp->array);
	// We fell off the MSB end.
      if (off >= vwidth) return true;
	// Trim the bits after the MSB
      if (off + (long)wid > vwidth) {
	    wid += vwidth - off - wid;
      } else if (off < 0 ) {
	      // We fell off the LSB end.
	    if ((unsigned)-off > wid ) return true;
	      // Trim the bits before the LSB
	    wid += off;
	    bit -= off;
	    off = 0;
      }

      assert(wid > 0);

      vvp_vector4_t value = vthread_bits_to_vector(thr, bit, wid);

      schedule_assign_array_word(cp->array, adr, off, value, delay);
      return true;
}

/* %assign/av/d <array>, <delay_idx>, <bit>
 * This generates an assignment event to an array. Index register 0
 * contains the width of the vector (and the word) and index register
 * 3 contains the canonical address of the word in memory. The named
 * index register contains the delay.
 */
bool of_ASSIGN_AVD(vthread_t thr, vvp_code_t cp)
{
      unsigned wid = thr->words[0].w_int;
      long off = thr->words[1].w_int;
      long adr = thr->words[3].w_int;
      vvp_time64_t delay = thr->words[cp->bit_idx[0]].w_uint;
      unsigned bit = cp->bit_idx[1];

      if (adr < 0) return true;

      long vwidth = get_array_word_size(cp->array);
	// We fell off the MSB end.
      if (off >= vwidth) return true;
	// Trim the bits after the MSB
      if (off + (long)wid > vwidth) {
	    wid += vwidth - off - wid;
      } else if (off < 0 ) {
	      // We fell off the LSB end.
	    if ((unsigned)-off > wid ) return true;
	      // Trim the bits before the LSB
	    wid += off;
	    bit -= off;
	    off = 0;
      }

      assert(wid > 0);

      vvp_vector4_t value = vthread_bits_to_vector(thr, bit, wid);

      schedule_assign_array_word(cp->array, adr, off, value, delay);
      return true;
}

bool of_ASSIGN_AVE(vthread_t thr, vvp_code_t cp)
{
      unsigned wid = thr->words[0].w_int;
      long off = thr->words[1].w_int;
      long adr = thr->words[3].w_int;
      unsigned bit = cp->bit_idx[0];

      if (adr < 0) return true;

      long vwidth = get_array_word_size(cp->array);
	// We fell off the MSB end.
      if (off >= vwidth) return true;
	// Trim the bits after the MSB
      if (off + (long)wid > vwidth) {
	    wid += vwidth - off - wid;
      } else if (off < 0 ) {
	      // We fell off the LSB end.
	    if ((unsigned)-off > wid ) return true;
	      // Trim the bits before the LSB
	    wid += off;
	    bit -= off;
	    off = 0;
      }

      assert(wid > 0);

      vvp_vector4_t value = vthread_bits_to_vector(thr, bit, wid);
	// If the count is zero then just put the value.
      if (thr->ecount == 0) {
	    schedule_assign_array_word(cp->array, adr, off, value, 0);
      } else {
	    schedule_evctl(cp->array, adr, value, off, thr->event, thr->ecount);
      }
      return true;
}

/*
 * This is %assign/v0 <label>, <delay>, <bit>
 * Index register 0 contains a vector width.
 */
bool of_ASSIGN_V0(vthread_t thr, vvp_code_t cp)
{
      unsigned wid = thr->words[0].w_int;
      assert(wid > 0);
      unsigned delay = cp->bit_idx[0];
      unsigned bit = cp->bit_idx[1];

      vvp_net_ptr_t ptr (cp->net, 0);
      if (bit >= 4) {
	      // If the vector is not a synthetic one, then have the
	      // scheduler pluck it directly out of my vector space.
	    schedule_assign_plucked_vector(ptr, delay, thr->bits4, bit, wid);
      } else {
	    vvp_vector4_t value = vthread_bits_to_vector(thr, bit, wid);
	    schedule_assign_plucked_vector(ptr, delay, value, 0, wid);
      }

      return true;
}

/*
 * This is %assign/v0/d <label>, <delay_idx>, <bit>
 * Index register 0 contains a vector width, and the named index
 * register contains the delay.
 */
bool of_ASSIGN_V0D(vthread_t thr, vvp_code_t cp)
{
      unsigned wid = thr->words[0].w_int;
      assert(wid > 0);

      vvp_time64_t delay = thr->words[cp->bit_idx[0]].w_uint;
      unsigned bit = cp->bit_idx[1];

      vvp_net_ptr_t ptr (cp->net, 0);

      if (bit >= 4) {
	    schedule_assign_plucked_vector(ptr, delay, thr->bits4, bit, wid);
      } else {
	    vvp_vector4_t value = vthread_bits_to_vector(thr, bit, wid);
	    schedule_assign_plucked_vector(ptr, delay, value, 0, wid);
      }

      return true;
}

/*
 * This is %assign/v0/e <label>, <bit>
 * Index register 0 contains a vector width.
 */
bool of_ASSIGN_V0E(vthread_t thr, vvp_code_t cp)
{
      assert(thr->event != 0);
      unsigned wid = thr->words[0].w_int;
      assert(wid > 0);
      unsigned bit = cp->bit_idx[0];

      vvp_net_ptr_t ptr (cp->net, 0);

      vvp_vector4_t value = vthread_bits_to_vector(thr, bit, wid);
	// If the count is zero then just put the value.
      if (thr->ecount == 0) {
	    schedule_assign_plucked_vector(ptr, 0, value, 0, wid);
      } else {
	    schedule_evctl(ptr, value, 0, 0, thr->event, thr->ecount);
      }

      thr->event = 0;
      thr->ecount = 0;

      return true;
}

/*
 * This is %assign/v0/x1 <label>, <delay>, <bit>
 * Index register 0 contains a vector part width.
 * Index register 1 contains the offset into the destination vector.
 */
bool of_ASSIGN_V0X1(vthread_t thr, vvp_code_t cp)
{
      unsigned wid = thr->words[0].w_int;
      long off = thr->words[1].w_int;
      unsigned delay = cp->bit_idx[0];
      unsigned bit = cp->bit_idx[1];

      vvp_signal_value*sig = dynamic_cast<vvp_signal_value*> (cp->net->fil);
      assert(sig);

	// We fell off the MSB end.
      if (off >= (long)sig->value_size()) return true;
      else if (off < 0 ) {
	      // We fell off the LSB end.
	    if ((unsigned)-off >= wid ) return true;
	      // Trim the bits before the LSB
	    wid += off;
	    bit -= off;
	    off = 0;
      }

      assert(wid > 0);

      vvp_vector4_t value = vthread_bits_to_vector(thr, bit, wid);

      vvp_net_ptr_t ptr (cp->net, 0);
      schedule_assign_vector(ptr, off, sig->value_size(), value, delay);

      return true;
}

/*
 * This is %assign/v0/x1/d <label>, <delayx>, <bit>
 * Index register 0 contains a vector part width.
 * Index register 1 contains the offset into the destination vector.
 */
bool of_ASSIGN_V0X1D(vthread_t thr, vvp_code_t cp)
{
      unsigned wid = thr->words[0].w_int;
      long off = thr->words[1].w_int;
      vvp_time64_t delay = thr->words[cp->bit_idx[0]].w_uint;
      unsigned bit = cp->bit_idx[1];

      vvp_signal_value*sig = dynamic_cast<vvp_signal_value*> (cp->net->fil);
      assert(sig);

	// We fell off the MSB end.
      if (off >= (long)sig->value_size()) return true;
      else if (off < 0 ) {
	      // We fell off the LSB end.
	    if ((unsigned)-off >= wid ) return true;
	      // Trim the bits before the LSB
	    wid += off;
	    bit -= off;
	    off = 0;
      }

      assert(wid > 0);

      vvp_vector4_t value = vthread_bits_to_vector(thr, bit, wid);

      vvp_net_ptr_t ptr (cp->net, 0);
      schedule_assign_vector(ptr, off, sig->value_size(), value, delay);

      return true;
}

/*
 * This is %assign/v0/x1/e <label>, <bit>
 * Index register 0 contains a vector part width.
 * Index register 1 contains the offset into the destination vector.
 */
bool of_ASSIGN_V0X1E(vthread_t thr, vvp_code_t cp)
{
      unsigned wid = thr->words[0].w_int;
      long off = thr->words[1].w_int;
      unsigned bit = cp->bit_idx[0];

      vvp_signal_value*sig = dynamic_cast<vvp_signal_value*> (cp->net->fil);
      assert(sig);

	// We fell off the MSB end.
      if (off >= (long)sig->value_size()) {
	    thr->event = 0;
	    thr->ecount = 0;
	    return true;
      } else if (off < 0 ) {
	      // We fell off the LSB end.
	    if ((unsigned)-off >= wid ) {
		  thr->event = 0;
		  thr->ecount = 0;
		  return true;
	    }
	      // Trim the bits before the LSB
	    wid += off;
	    bit -= off;
	    off = 0;
      }

      assert(wid > 0);

      vvp_vector4_t value = vthread_bits_to_vector(thr, bit, wid);

      vvp_net_ptr_t ptr (cp->net, 0);
	// If the count is zero then just put the value.
      if (thr->ecount == 0) {
	    schedule_assign_vector(ptr, off, sig->value_size(), value, 0);
      } else {
	    schedule_evctl(ptr, value, off, sig->value_size(), thr->event,
	                   thr->ecount);
      }

      thr->event = 0;
      thr->ecount = 0;

      return true;
}

/*
 * This is %assign/wr <vpi-label>, <delay>
 *
 * This assigns (after a delay) a value to a real variable. Use the
 * vpi_put_value function to do the assign, with the delay written
 * into the vpiInertialDelay carrying the desired delay.
 */
bool of_ASSIGN_WR(vthread_t thr, vvp_code_t cp)
{
      unsigned delay = cp->bit_idx[0];
      double value = thr->pop_real();
      s_vpi_time del;

      del.type = vpiSimTime;
      vpip_time_to_timestruct(&del, delay);

      __vpiHandle*tmp = cp->handle;

      t_vpi_value val;
      val.format = vpiRealVal;
      val.value.real = value;
      vpi_put_value(tmp, &val, &del, vpiTransportDelay);

      return true;
}

bool of_ASSIGN_WRD(vthread_t thr, vvp_code_t cp)
{
      vvp_time64_t delay = thr->words[cp->bit_idx[0]].w_uint;
      double value = thr->pop_real();
      s_vpi_time del;

      del.type = vpiSimTime;
      vpip_time_to_timestruct(&del, delay);

      __vpiHandle*tmp = cp->handle;

      t_vpi_value val;
      val.format = vpiRealVal;
      val.value.real = value;
      vpi_put_value(tmp, &val, &del, vpiTransportDelay);

      return true;
}

bool of_ASSIGN_WRE(vthread_t thr, vvp_code_t cp)
{
      assert(thr->event != 0);
      double value = thr->pop_real();
      __vpiHandle*tmp = cp->handle;

	// If the count is zero then just put the value.
      if (thr->ecount == 0) {
	    t_vpi_value val;

	    val.format = vpiRealVal;
	    val.value.real = value;
	    vpi_put_value(tmp, &val, 0, vpiNoDelay);
      } else {
	    schedule_evctl(tmp, value, thr->event, thr->ecount);
      }

      thr->event = 0;
      thr->ecount = 0;

      return true;
}

bool of_ASSIGN_X0(vthread_t, vvp_code_t)
{
#if 0
      unsigned char bit_val = thr_get_bit(thr, cp->bit_idx[1]);
      vvp_ipoint_t itmp = ipoint_index(cp->iptr, thr->words[0].w_int);
      schedule_assign(itmp, bit_val, cp->bit_idx[0]);
#else
      fprintf(stderr, "XXXX forgot how to implement %%assign/x0\n");
#endif
      return true;
}

bool of_BLEND(vthread_t thr, vvp_code_t cp)
{
      assert(cp->bit_idx[0] >= 4);

      unsigned idx1 = cp->bit_idx[0];
      unsigned idx2 = cp->bit_idx[1];

      for (unsigned idx = 0 ;  idx < cp->number ;  idx += 1) {
	    vvp_bit4_t lb = thr_get_bit(thr, idx1);
	    vvp_bit4_t rb = thr_get_bit(thr, idx2);

	    if (lb != rb)
		  thr_put_bit(thr, idx1, BIT4_X);

	    idx1 += 1;
	    if (idx2 >= 4)
		  idx2 += 1;
      }

      return true;
}

bool of_BLEND_WR(vthread_t thr, vvp_code_t)
{
      double f = thr->pop_real();
      double t = thr->pop_real();
      thr->push_real((t == f) ? t : 0.0);
      return true;
}

bool of_BREAKPOINT(vthread_t, vvp_code_t)
{
      return true;
}

/*
 * the %cassign/link instruction connects a source node to a
 * destination node. The destination node must be a signal, as it is
 * marked with the source of the cassign so that it may later be
 * unlinked without specifically knowing the source that this
 * instruction used.
 */
bool of_CASSIGN_LINK(vthread_t, vvp_code_t cp)
{
      vvp_net_t*dst = cp->net;
      vvp_net_t*src = cp->net2;

      vvp_fun_signal_base*sig
	    = dynamic_cast<vvp_fun_signal_base*>(dst->fun);
      assert(sig);

	/* Detect the special case that we are already continuous
	   assigning the source onto the destination. */
      if (sig->cassign_link == src)
	    return true;

	/* If there is an existing cassign driving this node, then
	   unlink it. We can have only 1 cassign at a time. */
      if (sig->cassign_link != 0) {
	    vvp_net_ptr_t tmp (dst, 1);
	    sig->cassign_link->unlink(tmp);
      }

      sig->cassign_link = src;

	/* Link the output of the src to the port[1] (the cassign
	   port) of the destination. */
      vvp_net_ptr_t dst_ptr (dst, 1);
      src->link(dst_ptr);

      return true;
}

/*
 * the %cassign/v instruction invokes a continuous assign of a
 * constant value to a signal. The instruction arguments are:
 *
 *     %cassign/v <net>, <base>, <wid> ;
 *
 * Where the <net> is the net label assembled into a vvp_net pointer,
 * and the <base> and <wid> are stashed in the bit_idx array.
 *
 * This instruction writes vvp_vector4_t values to port-1 of the
 * target signal.
 */
bool of_CASSIGN_V(vthread_t thr, vvp_code_t cp)
{
      vvp_net_t*net  = cp->net;
      unsigned  base = cp->bit_idx[0];
      unsigned  wid  = cp->bit_idx[1];

	/* Collect the thread bits into a vector4 item. */
      vvp_vector4_t value = vthread_bits_to_vector(thr, base, wid);

	/* set the value into port 1 of the destination. */
      vvp_net_ptr_t ptr (net, 1);
      vvp_send_vec4(ptr, value, 0);

      return true;
}

bool of_CASSIGN_WR(vthread_t thr, vvp_code_t cp)
{
      vvp_net_t*net  = cp->net;
      double value = thr->pop_real();

	/* Set the value into port 1 of the destination. */
      vvp_net_ptr_t ptr (net, 1);
      vvp_send_real(ptr, value, 0);

      return true;
}

bool of_CASSIGN_X0(vthread_t thr, vvp_code_t cp)
{
      vvp_net_t*net = cp->net;
      unsigned base = cp->bit_idx[0];
      unsigned wid = cp->bit_idx[1];

	// Implicitly, we get the base into the target vector from the
	// X0 register.
      long index = thr->words[0].w_int;

      vvp_signal_value*sig = dynamic_cast<vvp_signal_value*> (net->fil);

      if (index < 0 && (wid <= (unsigned)-index))
	    return true;

      if (index >= (long)sig->value_size())
	    return true;

      if (index < 0) {
	    wid -= (unsigned) -index;
	    index = 0;
      }

      if (index+wid > sig->value_size())
	    wid = sig->value_size() - index;

      vvp_vector4_t vector = vthread_bits_to_vector(thr, base, wid);

      vvp_net_ptr_t ptr (net, 1);
      vvp_send_vec4_pv(ptr, vector, index, wid, sig->value_size(), 0);

      return true;
}

bool of_CAST2(vthread_t thr, vvp_code_t cp)
{
      unsigned dst = cp->bit_idx[0];
      unsigned src = cp->bit_idx[1];
      unsigned wid = cp->number;

      thr_check_addr(thr, dst+wid-1);
      thr_check_addr(thr, src+wid-1);

      vvp_vector4_t res;
      switch (src) {
	  case 0:
	  case 2:
	  case 3:
	    res = vvp_vector4_t(wid, BIT4_0);
	    break;
	  case 1:
	    res = vvp_vector4_t(wid, BIT4_1);
	    break;
	  default:
	    res = vector2_to_vector4(vvp_vector2_t(vthread_bits_to_vector(thr, src, wid)), wid);
	    break;
      }

      thr->bits4.set_vec(dst, res);
      return true;
}

bool of_CMPS(vthread_t thr, vvp_code_t cp)
{
      vvp_bit4_t eq  = BIT4_1;
      vvp_bit4_t eeq = BIT4_1;
      vvp_bit4_t lt  = BIT4_0;

      unsigned idx1 = cp->bit_idx[0];
      unsigned idx2 = cp->bit_idx[1];

      const unsigned end1 = (idx1 < 4)? idx1 : idx1 + cp->number - 1;
      const unsigned end2 = (idx2 < 4)? idx2 : idx2 + cp->number - 1;

      if (end1 > end2)
	    thr_check_addr(thr, end1);
      else
	    thr_check_addr(thr, end2);

      const vvp_bit4_t sig1 = thr_get_bit(thr, end1);
      const vvp_bit4_t sig2 = thr_get_bit(thr, end2);

      for (unsigned idx = 0 ;  idx < cp->number ;  idx += 1) {
	    vvp_bit4_t lv = thr_get_bit(thr, idx1);
	    vvp_bit4_t rv = thr_get_bit(thr, idx2);

	    if (lv > rv) {
		  lt  = BIT4_0;
		  eeq = BIT4_0;
	    } else if (lv < rv) {
		  lt  = BIT4_1;
		  eeq = BIT4_0;
	    }
	    if (eq != BIT4_X) {
		  if ((lv == BIT4_0) && (rv != BIT4_0))
			eq = BIT4_0;
		  if ((lv == BIT4_1) && (rv != BIT4_1))
			eq = BIT4_0;
		  if (bit4_is_xz(lv) || bit4_is_xz(rv))
			eq = BIT4_X;
	    }

	    if (idx1 >= 4) idx1 += 1;
	    if (idx2 >= 4) idx2 += 1;
      }

      if (eq == BIT4_X)
	    lt = BIT4_X;
      else if ((sig1 == BIT4_1) && (sig2 == BIT4_0))
	    lt = BIT4_1;
      else if ((sig1 == BIT4_0) && (sig2 == BIT4_1))
	    lt = BIT4_0;

	/* Correct the lt bit to account for the sign of the parameters. */
      if (lt != BIT4_X) {
	      /* If the first is negative and the last positive, then
		 a < b for certain. */
	    if ((sig1 == BIT4_1) && (sig2 == BIT4_0))
		  lt = BIT4_1;

	      /* If the first is positive and the last negative, then
		 a > b for certain. */
	    if ((sig1 == BIT4_0) && (sig2 == BIT4_1))
		  lt = BIT4_0;
      }

      thr_put_bit(thr, 4, eq);
      thr_put_bit(thr, 5, lt);
      thr_put_bit(thr, 6, eeq);

      return true;
}

bool of_CMPSTR(vthread_t thr, vvp_code_t)
{
      string re = thr->pop_str();
      string le = thr->pop_str();

      int rc = strcmp(le.c_str(), re.c_str());

      vvp_bit4_t eq;
      vvp_bit4_t lt;

      if (rc == 0) {
	    eq = BIT4_1;
	    lt = BIT4_0;
      } else if (rc < 0) {
	    eq = BIT4_0;
	    lt = BIT4_1;
      } else {
	    eq = BIT4_0;
	    lt = BIT4_0;
      }

      thr_put_bit(thr, 4, eq);
      thr_put_bit(thr, 5, lt);

      return true;
}

bool of_CMPIS(vthread_t thr, vvp_code_t cp)
{
      vvp_bit4_t eq  = BIT4_1;
      vvp_bit4_t eeq = BIT4_1;
      vvp_bit4_t lt  = BIT4_0;

      unsigned idx1 = cp->bit_idx[0];
      unsigned imm  = cp->bit_idx[1];

      const unsigned end1 = (idx1 < 4)? idx1 : idx1 + cp->number - 1;
      thr_check_addr(thr, end1);
      const vvp_bit4_t sig1 = thr_get_bit(thr, end1);

      for (unsigned idx = 0 ;  idx < cp->number ;  idx += 1) {
	    vvp_bit4_t lv = thr_get_bit(thr, idx1);
	    vvp_bit4_t rv = (imm & 1)? BIT4_1 : BIT4_0;
	    imm >>= 1;

	    if (lv > rv) {
		  lt = BIT4_0;
		  eeq = BIT4_0;
	    } else if (lv < rv) {
		  lt = BIT4_1;
		  eeq = BIT4_0;
	    }
	    if (eq != BIT4_X) {
		  if ((lv == BIT4_0) && (rv != BIT4_0))
			eq = BIT4_0;
		  if ((lv == BIT4_1) && (rv != BIT4_1))
			eq = BIT4_0;
		  if (bit4_is_xz(lv) || bit4_is_xz(rv))
			eq = BIT4_X;
	    }

	    if (idx1 >= 4) idx1 += 1;
      }

      if (eq == BIT4_X)
	    lt = BIT4_X;
      else if (sig1 == BIT4_1)
	    lt = BIT4_1;

      thr_put_bit(thr, 4, eq);
      thr_put_bit(thr, 5, lt);
      thr_put_bit(thr, 6, eeq);

      return true;
}

/*
 * The of_CMPIU below punts to this function if there are any xz bits
 * in the vector part of the instruction. In this case we know that
 * there is at least 1 xz bit in the left expression (and there are
 * none in the imm value) so the eeq result must be false. Otherwise,
 * the eq result may be 0 or x, and the lt bit is x.
 */
static bool of_CMPIU_the_hard_way(vthread_t thr, vvp_code_t cp)
{

      unsigned idx1 = cp->bit_idx[0];
      unsigned long imm  = cp->bit_idx[1];
      unsigned wid  = cp->number;
      if (idx1 >= 4)
	    thr_check_addr(thr, idx1+wid-1);

      vvp_bit4_t lv = thr_get_bit(thr, idx1);
      vvp_bit4_t eq  = BIT4_1;
      for (unsigned idx = 0 ;  idx < wid ;  idx += 1) {
	    vvp_bit4_t rv = (imm & 1UL)? BIT4_1 : BIT4_0;
	    imm >>= 1UL;

	    if (bit4_is_xz(lv)) {
		  eq = BIT4_X;
	    } else if (lv != rv) {
		  eq = BIT4_0;
		  break;
	    }

	    if (idx1 >= 4) {
		  idx1 += 1;
		  if ((idx+1) < wid)
			lv = thr_get_bit(thr, idx1);
	    }
      }

      thr_put_bit(thr, 4, eq);
      thr_put_bit(thr, 5, BIT4_X);
      thr_put_bit(thr, 6, BIT4_0);

      return true;
}

bool of_CMPIU(vthread_t thr, vvp_code_t cp)
{
      unsigned addr = cp->bit_idx[0];
      unsigned long imm  = cp->bit_idx[1];
      unsigned wid  = cp->number;

      unsigned long*array = vector_to_array(thr, addr, wid);
	// If there are xz bits in the right hand expression, then we
	// have to do the compare the hard way. That is because even
	// though we know that eeq must be false (the immediate value
	// cannot have x or z bits) we don't know what the EQ or LT
	// bits will be.
      if (array == 0)
	    return of_CMPIU_the_hard_way(thr, cp);

      unsigned words = (wid+CPU_WORD_BITS-1) / CPU_WORD_BITS;
      vvp_bit4_t eq  = BIT4_1;
      vvp_bit4_t lt  = BIT4_0;
      for (unsigned idx = 0 ; idx < words ; idx += 1, imm = 0UL) {
	    if (array[idx] == imm)
		  continue;

	    eq = BIT4_0;
	    lt = (array[idx] < imm) ? BIT4_1 : BIT4_0;
      }

      delete[]array;

      thr_put_bit(thr, 4, eq);
      thr_put_bit(thr, 5, lt);
      thr_put_bit(thr, 6, eq);
      return true;
}

bool of_CMPU_the_hard_way(vthread_t thr, vvp_code_t cp)
{
      vvp_bit4_t eq = BIT4_1;
      vvp_bit4_t eeq = BIT4_1;

      unsigned idx1 = cp->bit_idx[0];
      unsigned idx2 = cp->bit_idx[1];

      for (unsigned idx = 0 ;  idx < cp->number ;  idx += 1) {
	    vvp_bit4_t lv = thr_get_bit(thr, idx1);
	    vvp_bit4_t rv = thr_get_bit(thr, idx2);

	    if (lv != rv)
		  eeq = BIT4_0;

	    if (eq==BIT4_1 && (bit4_is_xz(lv) || bit4_is_xz(rv)))
		  eq = BIT4_X;
	    if ((lv == BIT4_0) && (rv==BIT4_1))
		  eq = BIT4_0;
	    if ((lv == BIT4_1) && (rv==BIT4_0))
		  eq = BIT4_0;

	    if (eq == BIT4_0)
		  break;

	    if (idx1 >= 4) idx1 += 1;
	    if (idx2 >= 4) idx2 += 1;

      }

      thr_put_bit(thr, 4, eq);
      thr_put_bit(thr, 5, BIT4_X);
      thr_put_bit(thr, 6, eeq);

      return true;
}

bool of_CMPU(vthread_t thr, vvp_code_t cp)
{
      vvp_bit4_t eq = BIT4_1;
      vvp_bit4_t lt = BIT4_0;

      unsigned idx1 = cp->bit_idx[0];
      unsigned idx2 = cp->bit_idx[1];
      unsigned wid  = cp->number;

      unsigned long*larray = vector_to_array(thr, idx1, wid);
      if (larray == 0) return of_CMPU_the_hard_way(thr, cp);

      unsigned long*rarray = vector_to_array(thr, idx2, wid);
      if (rarray == 0) {
	    delete[]larray;
	    return of_CMPU_the_hard_way(thr, cp);
      }

      unsigned words = (wid+CPU_WORD_BITS-1) / CPU_WORD_BITS;

      for (unsigned wdx = 0 ; wdx < words ; wdx += 1) {
	    if (larray[wdx] == rarray[wdx])
		  continue;

	    eq = BIT4_0;
	    if (larray[wdx] < rarray[wdx])
		  lt = BIT4_1;
	    else
		  lt = BIT4_0;
      }

      delete[]larray;
      delete[]rarray;

      thr_put_bit(thr, 4, eq);
      thr_put_bit(thr, 5, lt);
      thr_put_bit(thr, 6, eq);

      return true;
}

bool of_CMPX(vthread_t thr, vvp_code_t cp)
{
      vvp_bit4_t eq = BIT4_1;

      unsigned idx1 = cp->bit_idx[0];
      unsigned idx2 = cp->bit_idx[1];

      for (unsigned idx = 0 ;  idx < cp->number ;  idx += 1) {
	    vvp_bit4_t lv = thr_get_bit(thr, idx1);
	    vvp_bit4_t rv = thr_get_bit(thr, idx2);

	    if ((lv != rv) && !bit4_is_xz(lv) && !bit4_is_xz(rv)) {
		  eq = BIT4_0;
		  break;
	    }

	    if (idx1 >= 4) idx1 += 1;
	    if (idx2 >= 4) idx2 += 1;
      }

      thr_put_bit(thr, 4, eq);

      return true;
}

bool of_CMPWR(vthread_t thr, vvp_code_t)
{
      double r = thr->pop_real();
      double l = thr->pop_real();

      vvp_bit4_t eq = (l == r)? BIT4_1 : BIT4_0;
      vvp_bit4_t lt = (l <  r)? BIT4_1 : BIT4_0;

      thr_put_bit(thr, 4, eq);
      thr_put_bit(thr, 5, lt);

      return true;
}

bool of_CMPWS(vthread_t thr, vvp_code_t cp)
{
      int64_t l = thr->words[cp->bit_idx[0]].w_int;
      int64_t r = thr->words[cp->bit_idx[1]].w_int;

      vvp_bit4_t eq = (l == r)? BIT4_1 : BIT4_0;
      vvp_bit4_t lt = (l <  r)? BIT4_1 : BIT4_0;

      thr_put_bit(thr, 4, eq);
      thr_put_bit(thr, 5, lt);

      return true;
}

bool of_CMPWU(vthread_t thr, vvp_code_t cp)
{
      uint64_t l = thr->words[cp->bit_idx[0]].w_uint;
      uint64_t r = thr->words[cp->bit_idx[1]].w_uint;

      vvp_bit4_t eq = (l == r)? BIT4_1 : BIT4_0;
      vvp_bit4_t lt = (l <  r)? BIT4_1 : BIT4_0;

      thr_put_bit(thr, 4, eq);
      thr_put_bit(thr, 5, lt);

      return true;
}

bool of_CMPZ(vthread_t thr, vvp_code_t cp)
{
      vvp_bit4_t eq = BIT4_1;

      unsigned idx1 = cp->bit_idx[0];
      unsigned idx2 = cp->bit_idx[1];

      for (unsigned idx = 0 ;  idx < cp->number ;  idx += 1) {
	    vvp_bit4_t lv = thr_get_bit(thr, idx1);
	    vvp_bit4_t rv = thr_get_bit(thr, idx2);

	    if ((lv != BIT4_Z) && (rv != BIT4_Z) && (lv != rv)) {
		  eq = BIT4_0;
		  break;
	    }

	    if (idx1 >= 4) idx1 += 1;
	    if (idx2 >= 4) idx2 += 1;
      }

      thr_put_bit(thr, 4, eq);

      return true;
}

/*
 *  %concat/str;
 */
bool of_CONCAT_STR(vthread_t thr, vvp_code_t)
{
      string text = thr->pop_str();
      thr->peek_str(0).append(text);
      return true;
}

/*
 *  %concati/str <string>;
 */
bool of_CONCATI_STR(vthread_t thr, vvp_code_t cp)
{
      const char*text = cp->text;
      thr->peek_str(0).append(text);
      return true;
}

bool of_CVT_RS(vthread_t thr, vvp_code_t cp)
{
      int64_t r = thr->words[cp->bit_idx[0]].w_int;
      thr->push_real( (double)(r) );

      return true;
}

bool of_CVT_RU(vthread_t thr, vvp_code_t cp)
{
      uint64_t r = thr->words[cp->bit_idx[0]].w_uint;
      thr->push_real( (double)(r) );

      return true;
}

bool of_CVT_RV(vthread_t thr, vvp_code_t cp)
{
      unsigned base = cp->bit_idx[0];
      unsigned wid = cp->bit_idx[1];
      vvp_vector4_t vector = vthread_bits_to_vector(thr, base, wid);
      double val;
      vector4_to_value(vector, val, false);
      thr->push_real(val);

      return true;
}

bool of_CVT_RV_S(vthread_t thr, vvp_code_t cp)
{
      unsigned base = cp->bit_idx[0];
      unsigned wid = cp->bit_idx[1];
      vvp_vector4_t vector = vthread_bits_to_vector(thr, base, wid);
      double val;
      vector4_to_value(vector, val, true);
      thr->push_real(val);

      return true;
}

/*
 * %cvt/sr <idx>
 * Pop the top value from the real stack, convert it to a 64bit signed
 * and save it to the indexed register.
 */
bool of_CVT_SR(vthread_t thr, vvp_code_t cp)
{
      double r = thr->pop_real();
      thr->words[cp->bit_idx[0]].w_int = i64round(r);

      return true;
}

bool of_CVT_UR(vthread_t thr, vvp_code_t cp)
{
      double r = thr->pop_real();
      if (r >= 0.0)
	    thr->words[cp->bit_idx[0]].w_uint = (uint64_t)floor(r+0.5);
      else
	    thr->words[cp->bit_idx[0]].w_uint = (uint64_t)ceil(r-0.5);

      return true;
}

/*
 * %cvt/vr <bit> <wid>
 */
bool of_CVT_VR(vthread_t thr, vvp_code_t cp)
{
      double r = thr->pop_real();
      unsigned base = cp->bit_idx[0];
      unsigned wid = cp->number;
      vvp_vector4_t tmp(wid, r);

	/* Make sure there is enough space for the new vector. */
      thr_check_addr(thr, base+wid-1);
      thr->bits4.set_vec(base, tmp);

      return true;
}

/*
 * This implements the %deassign instruction. All we do is write a
 * long(1) to port-3 of the addressed net. This turns off an active
 * continuous assign activated by %cassign/v
 */
bool of_DEASSIGN(vthread_t, vvp_code_t cp)
{
      vvp_net_t*net = cp->net;
      unsigned base  = cp->bit_idx[0];
      unsigned width = cp->bit_idx[1];

      vvp_signal_value*fil = dynamic_cast<vvp_signal_value*> (net->fil);
      assert(fil);
      vvp_fun_signal_vec*sig = dynamic_cast<vvp_fun_signal_vec*>(net->fun);
      assert(sig);

      if (base >= fil->value_size()) return true;
      if (base+width > fil->value_size()) width = fil->value_size() - base;

      bool full_sig = base == 0 && width == fil->value_size();

	// This is the net that is forcing me...
      if (vvp_net_t*src = sig->cassign_link) {
	    if (!full_sig) {
		  fprintf(stderr, "Sorry: when a signal is assigning a "
		          "register, I cannot deassign part of it.\n");
		  exit(1);
	    }
	      // And this is the pointer to be removed.
	    vvp_net_ptr_t dst_ptr (net, 1);
	    src->unlink(dst_ptr);
	    sig->cassign_link = 0;
      }

	/* Do we release all or part of the net? */
      if (full_sig) {
	    sig->deassign();
      } else {
	    sig->deassign_pv(base, width);
      }

      return true;
}

bool of_DEASSIGN_WR(vthread_t, vvp_code_t cp)
{
      vvp_net_t*net = cp->net;

      vvp_fun_signal_real*sig = dynamic_cast<vvp_fun_signal_real*>(net->fun);
      assert(sig);

	// This is the net that is forcing me...
      if (vvp_net_t*src = sig->cassign_link) {
	      // And this is the pointer to be removed.
	    vvp_net_ptr_t dst_ptr (net, 1);
	    src->unlink(dst_ptr);
	    sig->cassign_link = 0;
      }

      sig->deassign();

      return true;
}


/*
 * The delay takes two 32bit numbers to make up a 64bit time.
 *
 *   %delay <low>, <hig>
 */
bool of_DELAY(vthread_t thr, vvp_code_t cp)
{
      vvp_time64_t low = cp->bit_idx[0];
      vvp_time64_t hig = cp->bit_idx[1];

      vvp_time64_t res = 32;
      res = hig << res;
      res += low;

      schedule_vthread(thr, res);
      return false;
}

bool of_DELAYX(vthread_t thr, vvp_code_t cp)
{
      vvp_time64_t delay;

      assert(cp->number < 4);
      delay = thr->words[cp->number].w_uint;
      schedule_vthread(thr, delay);
      return false;
}

/* %delete/obj <label>
 *
 * This operator works by assigning a nil to the target object. This
 * causes any value that might be there to be garbage collected, thus
 * deleting the object.
 */
bool of_DELETE_OBJ(vthread_t thr, vvp_code_t cp)
{
	/* set the value into port 0 of the destination. */
      vvp_net_ptr_t ptr (cp->net, 0);
      vvp_send_object(ptr, vvp_object_t(), thr->wt_context);

      return true;
}

static bool do_disable(vthread_t thr, vthread_t match)
{
      bool flag = false;

	/* Pull the target thread out of its scope. */
      thr->parent_scope->threads.erase(thr);

	/* Turn the thread off by setting is program counter to
	   zero and setting an OFF bit. */
      thr->pc = codespace_null();
      thr->i_have_ended = 1;

	/* Turn off all the children of the thread. Simulate a %join
	   for as many times as needed to clear the results of all the
	   %forks that this thread has done. */
      while (!thr->children.empty()) {

	    vthread_t tmp = *(thr->children.begin());
	    assert(tmp);
	    assert(tmp->parent == thr);
	    thr->i_am_joining = 0;
	    if (do_disable(tmp, match))
		  flag = true;

	    vthread_reap(tmp);
      }

      if (thr->parent && thr->parent->i_am_joining) {
	      // If a parent is waiting in a %join, wake it up. Note
	      // that it is possible to be waiting in a %join yet
	      // already scheduled if multiple child threads are
	      // ending. So check if the thread is already scheduled
	      // before scheduling it again.
	    vthread_t parent = thr->parent;
	    parent->i_am_joining = 0;
	    if (! parent->i_have_ended)
		  schedule_vthread(parent, 0, true);

	      // Let the parent do the reaping.
	    vthread_reap(thr);

      } else if (thr->parent) {
	      /* If the parent is yet to %join me, let its %join
		 do the reaping. */
	      //assert(tmp->is_scheduled == 0);

      } else {
	      /* No parent at all. Goodbye. */
	    vthread_reap(thr);
      }

      return flag || (thr == match);
}

/*
 * Implement the %disable instruction by scanning the target scope for
 * all the target threads. Kill the target threads and wake up a
 * parent that is attempting a %join.
 */
bool of_DISABLE(vthread_t thr, vvp_code_t cp)
{
      struct __vpiScope*scope = (struct __vpiScope*)cp->handle;

      bool disabled_myself_flag = false;

      while (! scope->threads.empty()) {
	    set<vthread_t>::iterator cur = scope->threads.begin();

	      /* If I am disabling myself, then remember that fact so
		 that I can finish this statement differently. */
	    if (*cur == thr)
		  disabled_myself_flag = true;

	    if (do_disable(*cur, thr))
		  disabled_myself_flag = true;
      }

      return ! disabled_myself_flag;
}

/*
 * This function divides a 2-word number {high, a} by a 1-word
 * number. Assume that high < b.
 */
static unsigned long divide2words(unsigned long a, unsigned long b,
				  unsigned long high)
{
      unsigned long result = 0;
      while (high > 0) {
	    unsigned long tmp_result = ULONG_MAX / b;
	    unsigned long remain = ULONG_MAX % b;

	    remain += 1;
	    if (remain >= b) {
		  remain -= b;
		  tmp_result += 1;
	    }

	      // Now 0x1_0...0 = b*tmp_result + remain
	      // high*0x1_0...0 = high*(b*tmp_result + remain)
	      // high*0x1_0...0 = high*b*tmp_result + high*remain

	      // We know that high*0x1_0...0 >= high*b*tmp_result, and
	      // we know that high*0x1_0...0 > high*remain. Use
	      // high*remain as the remainder for another iteration,
	      // and add tmp_result*high into the current estimate of
	      // the result.
	    result += tmp_result * high;

	      // The new iteration starts with high*remain + a.
	    remain = multiply_with_carry(high, remain, high);
	    a += remain;
            if(a < remain)
              high += 1;

	      // Now result*b + {high,a} == the input {high,a}. It is
	      // possible that the new high >= 1. If so, it will
	      // certainly be less than high from the previous
	      // iteration. Do another iteration and it will shrink,
	      // eventually to 0.
      }

	// high is now 0, so a is the remaining remainder, so we can
	// finish off the integer divide with a simple a/b.

      return result + a/b;
}

static unsigned long* divide_bits(unsigned long*ap, unsigned long*bp, unsigned wid)
{
	// Do all our work a cpu-word at a time. The "words" variable
	// is the number of words of the wid.
      unsigned words = (wid+CPU_WORD_BITS-1) / CPU_WORD_BITS;

      unsigned btop = words-1;
      while (btop > 0 && bp[btop] == 0)
	    btop -= 1;

	// Detect divide by 0, and exit.
      if (btop==0 && bp[0]==0)
	    return 0;

	// The result array will eventually accumulate the result. The
	// diff array is a difference that we use in the intermediate.
      unsigned long*diff  = new unsigned long[words];
      unsigned long*result= new unsigned long[words];
      for (unsigned idx = 0 ; idx < words ; idx += 1)
	    result[idx] = 0;

      for (unsigned cur = words-btop ; cur > 0 ; cur -= 1) {
	    unsigned cur_ptr = cur-1;
	    unsigned long cur_res;
	    if (ap[cur_ptr+btop] >= bp[btop]) {
		  unsigned long high = 0;
		  if (cur_ptr+btop+1 < words)
			high = ap[cur_ptr+btop+1];
		  cur_res = divide2words(ap[cur_ptr+btop], bp[btop], high);

	    } else if (cur_ptr+btop+1 >= words) {
		  continue;

	    } else if (ap[cur_ptr+btop+1] == 0) {
		  continue;

	    } else {
		  cur_res = divide2words(ap[cur_ptr+btop], bp[btop],
					 ap[cur_ptr+btop+1]);
	    }

	      // cur_res is a guesstimate of the result this far. It
	      // may be 1 too big. (But it will also be >0) Try it,
	      // and if the difference comes out negative, then adjust.

	      // diff = (bp * cur_res)  << cur_ptr;
	    multiply_array_imm(diff+cur_ptr, bp, words-cur_ptr, cur_res);
	      // ap -= diff
	    unsigned long carry = 1;
	    for (unsigned idx = cur_ptr ; idx < words ; idx += 1)
		  ap[idx] = add_with_carry(ap[idx], ~diff[idx], carry);

	      // ap has the diff subtracted out of it. If cur_res was
	      // too large, then ap will turn negative. (We easily
	      // tell that ap turned negative by looking at
	      // carry&1. If it is 0, then it is *negative*.) In that
	      // case, we know that cur_res was too large by 1. Correct by
	      // adding 1b back in and reducing cur_res.
	    if ((carry&1) == 0) {
		    // Keep adding b back in until the remainder
		    // becomes positive again.
		  do {
			cur_res -= 1;
			carry = 0;
			for (unsigned idx = cur_ptr ; idx < words ; idx += 1)
			      ap[idx] = add_with_carry(ap[idx], bp[idx-cur_ptr], carry);
		  } while (carry == 0);
	    }

	    result[cur_ptr] = cur_res;
      }

	// Now ap contains the remainder and result contains the
	// desired result. We should find that:
	//  input-a = bp * result + ap;

      delete[]diff;
      return result;
}

bool of_DIV(vthread_t thr, vvp_code_t cp)
{
      unsigned adra = cp->bit_idx[0];
      unsigned adrb = cp->bit_idx[1];
      unsigned wid = cp->number;

      assert(adra >= 4);

      unsigned long*ap = vector_to_array(thr, adra, wid);
      if (ap == 0) {
	    vvp_vector4_t tmp(wid, BIT4_X);
	    thr->bits4.set_vec(adra, tmp);
	    return true;
      }

      unsigned long*bp = vector_to_array(thr, adrb, wid);
      if (bp == 0) {
	    delete[]ap;
	    vvp_vector4_t tmp(wid, BIT4_X);
	    thr->bits4.set_vec(adra, tmp);
	    return true;
      }

	// If the value fits in a single CPU word, then do it the easy way.
      if (wid <= CPU_WORD_BITS) {
	    if (bp[0] == 0) {
		  vvp_vector4_t tmp(wid, BIT4_X);
		  thr->bits4.set_vec(adra, tmp);
	    } else {
		  ap[0] /= bp[0];
		  thr->bits4.setarray(adra, wid, ap);
	    }
	    delete[]ap;
	    delete[]bp;
	    return true;
      }

      unsigned long*result = divide_bits(ap, bp, wid);
      if (result == 0) {
	    delete[]ap;
	    delete[]bp;
	    vvp_vector4_t tmp(wid, BIT4_X);
	    thr->bits4.set_vec(adra, tmp);
	    return true;
      }

	// Now ap contains the remainder and result contains the
	// desired result. We should find that:
	//  input-a = bp * result + ap;

      thr->bits4.setarray(adra, wid, result);
      delete[]ap;
      delete[]bp;
      delete[]result;
      return true;
}


static void negate_words(unsigned long*val, unsigned words)
{
      unsigned long carry = 1;
      for (unsigned idx = 0 ; idx < words ; idx += 1)
	    val[idx] = add_with_carry(0, ~val[idx], carry);
}

bool of_DIV_S(vthread_t thr, vvp_code_t cp)
{
      unsigned adra = cp->bit_idx[0];
      unsigned adrb = cp->bit_idx[1];
      unsigned wid = cp->number;
      unsigned words = (wid + CPU_WORD_BITS - 1) / CPU_WORD_BITS;

      assert(adra >= 4);

	// Get the values, left in right, in binary form. If there is
	// a problem with either (caused by an X or Z bit) then we
	// know right away that the entire result is X.
      unsigned long*ap = vector_to_array(thr, adra, wid);
      if (ap == 0) {
	    vvp_vector4_t tmp(wid, BIT4_X);
	    thr->bits4.set_vec(adra, tmp);
	    return true;
      }

      unsigned long*bp = vector_to_array(thr, adrb, wid);
      if (bp == 0) {
	    delete[]ap;
	    vvp_vector4_t tmp(wid, BIT4_X);
	    thr->bits4.set_vec(adra, tmp);
	    return true;
      }

	// Sign extend the bits in the array to fill out the array.
      unsigned long sign_mask = 0;
      if (unsigned long sign_bits = (words*CPU_WORD_BITS) - wid) {
	    sign_mask = -1UL << (CPU_WORD_BITS-sign_bits);
	    if (ap[words-1] & (sign_mask>>1))
		  ap[words-1] |= sign_mask;
	    if (bp[words-1] & (sign_mask>>1))
		  bp[words-1] |= sign_mask;
      }

	// If the value fits in a single word, then use the native divide.
      if (wid <= CPU_WORD_BITS) {
	    if (bp[0] == 0) {
		  vvp_vector4_t tmp(wid, BIT4_X);
		  thr->bits4.set_vec(adra, tmp);
	    } else {
		  long tmpa = (long) ap[0];
		  long tmpb = (long) bp[0];
		  long res = tmpa / tmpb;
		  ap[0] = ((unsigned long)res) & ~sign_mask;
		  thr->bits4.setarray(adra, wid, ap);
	    }
	    delete[]ap;
	    delete[]bp;
	    return true;
      }

	// We need to the actual division to positive integers. Make
	// them positive here, and remember the negations.
      bool negate_flag = false;
      if ( ((long) ap[words-1]) < 0 ) {
	    negate_flag = true;
	    negate_words(ap, words);
      }
      if ( ((long) bp[words-1]) < 0 ) {
	    negate_flag ^= true;
	    negate_words(bp, words);
      }

      unsigned long*result = divide_bits(ap, bp, wid);
      if (result == 0) {
	    delete[]ap;
	    delete[]bp;
	    vvp_vector4_t tmp(wid, BIT4_X);
	    thr->bits4.set_vec(adra, tmp);
	    return true;
      }

      if (negate_flag) {
	    negate_words(result, words);
      }

      result[words-1] &= ~sign_mask;

      thr->bits4.setarray(adra, wid, result);
      delete[]ap;
      delete[]bp;
      delete[]result;
      return true;
}

bool of_DIV_WR(vthread_t thr, vvp_code_t)
{
      double r = thr->pop_real();
      double l = thr->pop_real();
      thr->push_real(l / r);

      return true;
}

bool of_DUP_REAL(vthread_t thr, vvp_code_t)
{
      thr->push_real(thr->peek_real(0));
      return true;
}

/*
 * This terminates the current thread. If there is a parent who is
 * waiting for me to die, then I schedule it. At any rate, I mark
 * myself as a zombie by setting my pc to 0.
 *
 * It is possible for this thread to have children at this %end. This
 * means that my child is really my sibling created by my parent, and
 * my parent will do the proper %joins in due course. For example:
 *
 *     %fork child_1, test;
 *     %fork child_2, test;
 *     ... parent code ...
 *     %join;
 *     %join;
 *     %end;
 *
 *   child_1 ;
 *     %end;
 *   child_2 ;
 *     %end;
 *
 * In this example, the main thread creates threads child_1 and
 * child_2. It is possible that this thread is child_2, so there is a
 * parent pointer and a child pointer, even though I did no
 * %forks or %joins. This means that I have a ->child pointer and a
 * ->parent pointer.
 *
 * If the main thread has executed the first %join, then it is waiting
 * for me, and I will be reaped right away.
 *
 * If the main thread has not executed a %join yet, then this thread
 * becomes a zombie. The main thread executes its %join eventually,
 * reaping me at that time.
 *
 * It does not matter the order that child_1 and child_2 threads call
 * %end -- child_2 will be reaped by the first %join, and child_1 will
 * be reaped by the second %join.
 */
bool of_END(vthread_t thr, vvp_code_t)
{
      assert(! thr->waiting_for_event);
      thr->i_have_ended = 1;
      thr->pc = codespace_null();

	/* If I have a parent who is waiting for me, then mark that I
	   have ended, and schedule that parent. Also, finish the
	   %join for the parent. */
      if (thr->parent && thr->parent->i_am_joining) {
	    vthread_t tmp = thr->parent;

	      // Detect that the parent is waiting on an automatic
	      // thread. Automatic threads must be reaped first. If
	      // the parent is waiting on an auto (other than me) then
	      // go into zombie state to be picked up later.
	    if (!test_joinable(tmp, thr))
		  return false;

	    tmp->i_am_joining = 0;
	    schedule_vthread(tmp, 0, true);
	    do_join(tmp, thr);
	    return false;
      }

	/* If I have no parents, then no one can %join me and there is
	   no reason to stick around. This can happen, for example if
	   I am an ``initial'' thread.

	   If I have children at this point, then I must have been the
	   main thread (there is no other parent) and an error (not
	   enough %joins) has been detected. */
      if (thr->parent == 0) {
	    assert(thr->children.empty());
	    vthread_reap(thr);
	    return false;
      }

	/* If I make it this far, then I have a parent who may wish
	   to %join me. Remain a zombie so that it can. */

      return false;
}

bool of_EVCTL(vthread_t thr, vvp_code_t cp)
{
      assert(thr->event == 0 && thr->ecount == 0);
      thr->event = cp->net;
      thr->ecount = thr->words[cp->bit_idx[0]].w_uint;
      return true;
}
bool of_EVCTLC(vthread_t thr, vvp_code_t)
{
      thr->event = 0;
      thr->ecount = 0;
      return true;
}

bool of_EVCTLI(vthread_t thr, vvp_code_t cp)
{
      assert(thr->event == 0 && thr->ecount == 0);
      thr->event = cp->net;
      thr->ecount = cp->bit_idx[0];
      return true;
}

bool of_EVCTLS(vthread_t thr, vvp_code_t cp)
{
      assert(thr->event == 0 && thr->ecount == 0);
      thr->event = cp->net;
      int64_t val = thr->words[cp->bit_idx[0]].w_int;
      if (val < 0) val = 0;
      thr->ecount = val;
      return true;
}

/*
 * the %force/link instruction connects a source node to a
 * destination node. The destination node must be a signal, as it is
 * marked with the source of the force so that it may later be
 * unlinked without specifically knowing the source that this
 * instruction used.
 */
bool of_FORCE_LINK(vthread_t, vvp_code_t cp)
{
      vvp_net_t*dst = cp->net;
      vvp_net_t*src = cp->net2;

      assert(dst->fil);
      dst->fil->force_link(dst, src);

      return true;
}

/*
 * The %force/v instruction invokes a force assign of a constant value
 * to a signal. The instruction arguments are:
 *
 *     %force/v <net>, <base>, <wid> ;
 *
 * where the <net> is the net label assembled into a vvp_net pointer,
 * and the <base> and <wid> are stashed in the bit_idx array.
 *
 * The instruction writes a vvp_vector4_t value to port-2 of the
 * target signal.
 */
bool of_FORCE_V(vthread_t thr, vvp_code_t cp)
{
      vvp_net_t*net  = cp->net;
      unsigned  base = cp->bit_idx[0];
      unsigned  wid  = cp->bit_idx[1];

	/* Collect the thread bits into a vector4 item. */
      vvp_vector4_t value = vthread_bits_to_vector(thr, base, wid);

	/* Send the force value to the filter on the node. */

      assert(net->fil);
      if (value.size() != net->fil->filter_size())
	    value = coerce_to_width(value, net->fil->filter_size());

      net->force_vec4(value, vvp_vector2_t(vvp_vector2_t::FILL1, net->fil->filter_size()));

      return true;
}

bool of_FORCE_WR(vthread_t thr, vvp_code_t cp)
{
      vvp_net_t*net  = cp->net;
      double value = thr->pop_real();

      net->force_real(value, vvp_vector2_t(vvp_vector2_t::FILL1, 1));

      return true;
}


bool of_FORCE_X0(vthread_t thr, vvp_code_t cp)
{
      vvp_net_t*net = cp->net;
      unsigned base = cp->bit_idx[0];
      unsigned wid = cp->bit_idx[1];

      assert(net->fil);

	// Implicitly, we get the base into the target vector from the
	// X0 register.
      long index = thr->words[0].w_int;

      if (index < 0 && (wid <= (unsigned)-index))
	    return true;

      if (index < 0) {
	    wid -= (unsigned) -index;
	    index = 0;
      }

      unsigned use_size = net->fil->filter_size();


      if (index >= (long)use_size)
	    return true;

      if (index+wid > use_size)
	    wid = use_size - index;

      vvp_vector2_t mask(vvp_vector2_t::FILL0, use_size);
      for (unsigned idx = 0 ; idx < wid ; idx += 1)
	    mask.set_bit(index+idx, 1);

      vvp_vector4_t vector = vthread_bits_to_vector(thr, base, wid);
      vvp_vector4_t value(use_size, BIT4_Z);
      value.set_vec(index, vector);

      net->force_vec4(value, mask);

      return true;
}

/*
 * The %fork instruction causes a new child to be created and pushed
 * in front of any existing child. This causes the new child to be
 * added to the list of children, and for me to be the parent of the
 * new child.
 */
bool of_FORK(vthread_t thr, vvp_code_t cp)
{
      vthread_t child = vthread_new(cp->cptr2, cp->scope);

      if (cp->scope->is_automatic) {
              /* The context allocated for this child is the top entry
                 on the write context stack. */
            child->wt_context = thr->wt_context;
            child->rd_context = thr->wt_context;

	    thr->automatic_children.insert(child);
      }

      child->parent = thr;
      thr->children.insert(child);

	/* If the new child was created to evaluate a function,
	   run it immediately, then return to this thread. */
      if (cp->scope->get_type_code() == vpiFunction) {
	    child->is_scheduled = 1;
	    vthread_run(child);
            running_thread = thr;
      } else {
	    schedule_vthread(child, 0, true);
      }

      return true;
}

bool of_FREE(vthread_t thr, vvp_code_t cp)
{
        /* Pop the child context from the read context stack. */
      vvp_context_t child_context = thr->rd_context;
      thr->rd_context = vvp_get_stacked_context(child_context);

        /* Free the context. */
      vthread_free_context(child_context, cp->scope);

      return true;
}

static bool of_INV_wide(vthread_t thr, vvp_code_t cp)
{
      unsigned idx1 = cp->bit_idx[0];
      unsigned wid = cp->bit_idx[1];

      vvp_vector4_t val = vthread_bits_to_vector(thr, idx1, wid);
      thr->bits4.set_vec(idx1, ~val);

      return true;
}

static bool of_INV_narrow(vthread_t thr, vvp_code_t cp)
{
      unsigned idx1 = cp->bit_idx[0];
      unsigned wid = cp->bit_idx[1];

      for (unsigned idx = 0 ; idx < wid ; idx += 1) {
	    vvp_bit4_t lb = thr_get_bit(thr, idx1);
	    thr_put_bit(thr, idx1, ~lb);
	    idx1 += 1;
      }

      return true;
}

bool of_INV(vthread_t thr, vvp_code_t cp)
{
      assert(cp->bit_idx[0] >= 4);

      if (cp->number <= 4)
	    cp->opcode = &of_INV_narrow;
      else
	    cp->opcode = &of_INV_wide;

      return cp->opcode(thr, cp);
}


/*
 * Index registers, arithmetic.
 */

static inline int64_t get_as_64_bit(uint32_t low_32, uint32_t high_32)
{
      int64_t low = low_32;
      int64_t res = high_32;

      res <<= 32;
      res |= low;
      return res;
}

bool of_IX_ADD(vthread_t thr, vvp_code_t cp)
{
      thr->words[cp->number].w_int += get_as_64_bit(cp->bit_idx[0],
                                                    cp->bit_idx[1]);
      return true;
}

bool of_IX_SUB(vthread_t thr, vvp_code_t cp)
{
      thr->words[cp->number].w_int -= get_as_64_bit(cp->bit_idx[0],
                                                    cp->bit_idx[1]);
      return true;
}

bool of_IX_MUL(vthread_t thr, vvp_code_t cp)
{
      thr->words[cp->number].w_int *= get_as_64_bit(cp->bit_idx[0],
                                                    cp->bit_idx[1]);
      return true;
}

bool of_IX_LOAD(vthread_t thr, vvp_code_t cp)
{
      thr->words[cp->number].w_int = get_as_64_bit(cp->bit_idx[0],
                                                   cp->bit_idx[1]);
      return true;
}

/*
 * Load a vector into an index register. The format of the
 * opcode is:
 *
 *   %ix/get <ix>, <base>, <wid>
 *
 * where <ix> is the index register, <base> is the base of the
 * vector and <wid> is the width in bits.
 *
 * Index registers only hold binary values, so if any of the
 * bits of the vector are x or z, then set the value to 0,
 * set bit[4] to 1, and give up.
 */

static uint64_t vector_to_index(vthread_t thr, unsigned base,
                                unsigned width, bool signed_flag)
{
      uint64_t v = 0;
      bool unknown_flag = false;

      vvp_bit4_t vv = BIT4_0;
      for (unsigned i = 0 ;  i < width ;  i += 1) {
	    vv = thr_get_bit(thr, base);
	    if (bit4_is_xz(vv)) {
		  v = 0UL;
		  unknown_flag = true;
		  break;
	    }

	    v |= (uint64_t) vv << i;

	    if (base >= 4)
		  base += 1;
      }

	/* Extend to fill the integer value. */
      if (signed_flag && !unknown_flag) {
	    uint64_t pad = vv;
	    for (unsigned i = width ; i < 8*sizeof(v) ;  i += 1) {
		  v |= pad << i;
	    }
      }

	/* Set bit 4 as a flag if the input is unknown. */
      thr_put_bit(thr, 4, unknown_flag ? BIT4_1 : BIT4_0);

      return v;
}

bool of_IX_GET(vthread_t thr, vvp_code_t cp)
{
      unsigned index = cp->bit_idx[0];
      unsigned base  = cp->bit_idx[1];
      unsigned width = cp->number;

      thr->words[index].w_uint = vector_to_index(thr, base, width, false);
      return true;
}

bool of_IX_GET_S(vthread_t thr, vvp_code_t cp)
{
      unsigned index = cp->bit_idx[0];
      unsigned base  = cp->bit_idx[1];
      unsigned width = cp->number;

      thr->words[index].w_int = vector_to_index(thr, base, width, true);
      return true;
}

bool of_IX_GETV(vthread_t thr, vvp_code_t cp)
{
      unsigned index = cp->bit_idx[0];
      vvp_net_t*net = cp->net;

      vvp_signal_value*sig = dynamic_cast<vvp_signal_value*>(net->fil);
      if (sig == 0) {
	    assert(net->fil);
	    cerr << "%%ix/getv error: Net arg not a vector signal? "
		 << typeid(*net->fil).name() << endl;
      }
      assert(sig);

      vvp_vector4_t vec;
      sig->vec4_value(vec);
      uint64_t val;
      bool known_flag = vector4_to_value(vec, val);

      if (known_flag)
	    thr->words[index].w_uint = val;
      else
	    thr->words[index].w_uint = 0;

	/* Set bit 4 as a flag if the input is unknown. */
      thr_put_bit(thr, 4, known_flag ? BIT4_0 : BIT4_1);

      return true;
}

bool of_IX_GETV_S(vthread_t thr, vvp_code_t cp)
{
      unsigned index = cp->bit_idx[0];
      vvp_net_t*net = cp->net;

      vvp_signal_value*sig = dynamic_cast<vvp_signal_value*>(net->fil);
      if (sig == 0) {
	    assert(net->fil);
	    cerr << "%%ix/getv/s error: Net arg not a vector signal? "
		 << "fun=" << typeid(*net->fil).name()
		 << ", fil=" << (net->fil? typeid(*net->fil).name() : "<>")
		 << endl;
      }
      assert(sig);

      vvp_vector4_t vec;
      sig->vec4_value(vec);
      int64_t val;
      bool known_flag = vector4_to_value(vec, val, true, true);

      if (known_flag)
	    thr->words[index].w_int = val;
      else
	    thr->words[index].w_int = 0;

	/* Set bit 4 as a flag if the input is unknown. */
      thr_put_bit(thr, 4, known_flag ? BIT4_0 : BIT4_1);

      return true;
}

/*
 * The various JMP instruction work simply by pulling the new program
 * counter from the instruction and resuming. If the jump is
 * conditional, then test the bit for the expected value first.
 */
bool of_JMP(vthread_t thr, vvp_code_t cp)
{
      thr->pc = cp->cptr;

	/* Normally, this returns true so that the processor just
	   keeps going to the next instruction. However, if there was
	   a $stop or vpiStop, returning false here can break the
	   simulation out of a hung loop. */
      if (schedule_stopped()) {
	    schedule_vthread(thr, 0, false);
	    return false;
      }

      return true;
}

bool of_JMP0(vthread_t thr, vvp_code_t cp)
{
      if (thr_get_bit(thr, cp->bit_idx[0]) == 0)
	    thr->pc = cp->cptr;

	/* Normally, this returns true so that the processor just
	   keeps going to the next instruction. However, if there was
	   a $stop or vpiStop, returning false here can break the
	   simulation out of a hung loop. */
      if (schedule_stopped()) {
	    schedule_vthread(thr, 0, false);
	    return false;
      }

      return true;
}

bool of_JMP0XZ(vthread_t thr, vvp_code_t cp)
{
      if (thr_get_bit(thr, cp->bit_idx[0]) != BIT4_1)
	    thr->pc = cp->cptr;

	/* Normally, this returns true so that the processor just
	   keeps going to the next instruction. However, if there was
	   a $stop or vpiStop, returning false here can break the
	   simulation out of a hung loop. */
      if (schedule_stopped()) {
	    schedule_vthread(thr, 0, false);
	    return false;
      }

      return true;
}

bool of_JMP1(vthread_t thr, vvp_code_t cp)
{
      if (thr_get_bit(thr, cp->bit_idx[0]) == 1)
	    thr->pc = cp->cptr;

	/* Normally, this returns true so that the processor just
	   keeps going to the next instruction. However, if there was
	   a $stop or vpiStop, returning false here can break the
	   simulation out of a hung loop. */
      if (schedule_stopped()) {
	    schedule_vthread(thr, 0, false);
	    return false;
      }

      return true;
}

/*
 * The %join instruction causes the thread to wait for one child
 * to die.  If a child is already dead (and a zombie) then I reap
 * it and go on. Otherwise, I mark myself as waiting in a join so that
 * children know to wake me when they finish.
 */

static bool test_joinable(vthread_t thr, vthread_t child)
{
      set<vthread_t>::iterator auto_cur = thr->automatic_children.find(child);
      if (!thr->automatic_children.empty() && auto_cur == thr->automatic_children.end())
	    return false;

      return true;
}

static void do_join(vthread_t thr, vthread_t child)
{
      assert(child->parent == thr);

        /* If the immediate child thread is in an automatic scope... */
      if (thr->automatic_children.erase(child) != 0) {
              /* and is the top level task/function thread... */
            if (thr->wt_context != thr->rd_context) {
                    /* Pop the child context from the write context stack. */
                  vvp_context_t child_context = thr->wt_context;
                  thr->wt_context = vvp_get_stacked_context(child_context);

                    /* Push the child context onto the read context stack */
                  vvp_set_stacked_context(child_context, thr->rd_context);
                  thr->rd_context = child_context;
            }
      }

      vthread_reap(child);
}

bool of_JOIN(vthread_t thr, vvp_code_t)
{
      assert( !thr->i_am_joining );
      assert( !thr->children.empty());

	// Are there any children that have already ended? If so, then
	// join with that one.
      for (set<vthread_t>::iterator cur = thr->children.begin()
		 ; cur != thr->children.end() ; ++cur) {
	    vthread_t curp = *cur;
	    if (!curp->i_have_ended)
		  continue;

	    if (!test_joinable(thr, curp))
		  continue;

	      // found something!
	    do_join(thr, curp);
	    return true;
      }

	// Otherwise, tell my children to awaken me when they end,
	// then pause.
      thr->i_am_joining = 1;
      return false;
}

/*
 * This %join/detach <n> instruction causes the thread to detach
 * threads that were created by an earlier %fork.
 */
bool of_JOIN_DETACH(vthread_t thr, vvp_code_t cp)
{
      unsigned long count = cp->number;

      assert(thr->automatic_children.empty());
      assert(count == thr->children.size());

      while (!thr->children.empty()) {
	    vthread_t child = *thr->children.begin();
	    assert(child->parent == thr);

	      // We cannot detach automatic tasks/functions
	    assert(child->wt_context == 0);
	    if (child->i_have_ended) {
		    // If the child has already ended, then reap it.
		  vthread_reap(child);

	    } else {
		  thr->children.erase(child);
		  child->parent = 0;
	    }
      }

      return true;
}

/*
 * %load/ar <array-label>, <index>;
*/
bool of_LOAD_AR(vthread_t thr, vvp_code_t cp)
{
      unsigned idx = cp->bit_idx[0];
      unsigned adr = thr->words[idx].w_int;
      double word;

	/* The result is 0.0 if the address is undefined. */
      if (thr_get_bit(thr, 4) == BIT4_1) {
	    word = 0.0;
      } else {
	    word = array_get_word_r(cp->array, adr);
      }

      thr->push_real(word);
      return true;
}

/*
 * %load/av <bit>, <array-label>, <wid> ;
 *
 * <bit> is the thread bit address for the result
 * <array-label> is the array to access, and
 * <wid> is the width of the word to read.
 *
 * The address of the word in the array is in index register 3.
 */
bool of_LOAD_AV(vthread_t thr, vvp_code_t cp)
{
      unsigned bit = cp->bit_idx[0];
      unsigned wid = cp->bit_idx[1];
      unsigned adr = thr->words[3].w_int;

	/* Check the address once, before we scan the vector. */
      thr_check_addr(thr, bit+wid-1);

	/* The result is 'bx if the address is undefined. */
      if (thr_get_bit(thr, 4) == BIT4_1) {
	    vvp_vector4_t tmp (wid, BIT4_X);
	    thr->bits4.set_vec(bit, tmp);
	    return true;
      }

      vvp_vector4_t word = array_get_word(cp->array, adr);

      if (word.size() > wid)
	    word.resize(wid);

	/* Copy the vector bits into the bits4 vector. Do the copy
	   directly to skip the excess calls to thr_check_addr. */
      thr->bits4.set_vec(bit, word);

	/* If the source is shorter than the desired width, then pad
	   with BIT4_X values. */
      for (unsigned idx = word.size() ; idx < wid ; idx += 1)
	    thr->bits4.set_bit(bit+idx, BIT4_X);

      return true;
}

/*
 * %load/dar <bit>, <array-label>, <index>;
*/
bool of_LOAD_DAR(vthread_t thr, vvp_code_t cp)
{
      unsigned bit = cp->bit_idx[0];
      unsigned wid = cp->bit_idx[1];
      unsigned adr = thr->words[3].w_int;
      vvp_net_t*net = cp->net;

      assert(net);
      vvp_fun_signal_object*obj = dynamic_cast<vvp_fun_signal_object*> (net->fun);
      assert(obj);

      vvp_darray*darray = obj->get_object().peek<vvp_darray>();
      assert(darray);

      vvp_vector4_t word;
      darray->get_word(adr, word);
      assert(word.size() == wid);

      thr_check_addr(thr, bit+word.size());
      thr->bits4.set_vec(bit, word);

      return true;
}

/*
 * %load/dar/r <array-label>;
 */
bool of_LOAD_DAR_R(vthread_t thr, vvp_code_t cp)
{
      unsigned adr = thr->words[3].w_int;
      vvp_net_t*net = cp->net;

      assert(net);
      vvp_fun_signal_object*obj = dynamic_cast<vvp_fun_signal_object*> (net->fun);
      assert(obj);

      vvp_darray*darray = obj->get_object().peek<vvp_darray>();
      assert(darray);

      double word;
      darray->get_word(adr, word);

      thr->push_real(word);
      return true;
}

bool of_LOAD_DAR_STR(vthread_t thr, vvp_code_t cp)
{
      unsigned adr = thr->words[3].w_int;
      vvp_net_t*net = cp->net;

      assert(net);
      vvp_fun_signal_object*obj = dynamic_cast<vvp_fun_signal_object*> (net->fun);
      assert(obj);

      vvp_darray*darray = obj->get_object().peek<vvp_darray>();
      assert(darray);

      string word;
      darray->get_word(adr, word);
      thr->push_str(word);

      return true;
}

/*
 * %load/vp0, %load/vp0/s, %load/avp0 and %load/avp0/s share this function.
 */
#if (SIZEOF_UNSIGNED_LONG >= 8)
# define CPU_WORD_STRIDE CPU_WORD_BITS - 1  // avoid a warning
#else
# define CPU_WORD_STRIDE CPU_WORD_BITS
#endif
static void load_vp0_common(vthread_t thr, vvp_code_t cp, const vvp_vector4_t&sig_value)
{
      unsigned bit = cp->bit_idx[0];
      unsigned wid = cp->bit_idx[1];
      int64_t addend = thr->words[0].w_int;

	/* Check the address once, before we scan the vector. */
      thr_check_addr(thr, bit+wid-1);

      unsigned long*val = sig_value.subarray(0, wid);
      if (val == 0) {
	    vvp_vector4_t tmp(wid, BIT4_X);
	    thr->bits4.set_vec(bit, tmp);
	    return;
      }

      unsigned words = (wid + CPU_WORD_BITS - 1) / CPU_WORD_BITS;
      unsigned long carry = 0;
      unsigned long imm = addend;
      for (unsigned idx = 0 ; idx < words ; idx += 1) {
            val[idx] = add_with_carry(val[idx], imm, carry);
            addend >>= CPU_WORD_STRIDE;
            imm = addend;
      }

	/* Copy the vector bits into the bits4 vector. Do the copy
	   directly to skip the excess calls to thr_check_addr. */
      thr->bits4.setarray(bit, wid, val);
      delete[]val;
}

/*
 * %load/avp0 <bit>, <array-label>, <wid> ;
 *
 * <bit> is the thread bit address for the result
 * <array-label> is the array to access, and
 * <wid> is the width of the word to read.
 *
 * The address of the word in the array is in index register 3.
 * An integer value from index register 0 is added to the value.
 */
bool of_LOAD_AVP0(vthread_t thr, vvp_code_t cp)
{
      unsigned wid = cp->bit_idx[1];
      unsigned adr = thr->words[3].w_int;

	/* The result is 'bx if the address is undefined. */
      if (thr_get_bit(thr, 4) == BIT4_1) {
	    unsigned bit = cp->bit_idx[0];
	    thr_check_addr(thr, bit+wid-1);
	    vvp_vector4_t tmp (wid, BIT4_X);
	    thr->bits4.set_vec(bit, tmp);
	    return true;
      }

        /* We need a vector this wide to make the math work correctly.
         * Copy the base bits into the vector, but keep the width. */
      vvp_vector4_t sig_value(wid, BIT4_0);
      sig_value.copy_bits(array_get_word(cp->array, adr));

      load_vp0_common(thr, cp, sig_value);
      return true;
}

bool of_LOAD_AVP0_S(vthread_t thr, vvp_code_t cp)
{
      unsigned wid = cp->bit_idx[1];
      unsigned adr = thr->words[3].w_int;

	/* The result is 'bx if the address is undefined. */
      if (thr_get_bit(thr, 4) == BIT4_1) {
	    unsigned bit = cp->bit_idx[0];
	    thr_check_addr(thr, bit+wid-1);
	    vvp_vector4_t tmp (wid, BIT4_X);
	    thr->bits4.set_vec(bit, tmp);
	    return true;
      }

      vvp_vector4_t tmp (array_get_word(cp->array, adr));

        /* We need a vector this wide to make the math work correctly.
         * Copy the base bits into the vector, but keep the width. */
      vvp_vector4_t sig_value(wid, tmp.value(tmp.size()-1));
      sig_value.copy_bits(tmp);

      load_vp0_common(thr, cp, sig_value);
      return true;
}

/*
 * %load/avx.p <bit>, <array-label>, <idx> ;
 *
 * <bit> is the thread bit address for the result
 * <array-label> is the array to access, and
 * <wid> is the width of the word to read.
 *
 * The address of the word in the array is in index register 3.
 */
bool of_LOAD_AVX_P(vthread_t thr, vvp_code_t cp)
{
      unsigned bit = cp->bit_idx[0];
      unsigned index = cp->bit_idx[1];
      unsigned adr = thr->words[3].w_int;

	/* The result is 'bx if the address is undefined. */
      if (thr_get_bit(thr, 4) == BIT4_1) {
	    thr_put_bit(thr, bit, BIT4_X);
	    return true;
      }

      long use_index = thr->words[index].w_int;

      vvp_vector4_t word = array_get_word(cp->array, adr);

      if ((use_index >= (long)word.size()) || (use_index < 0)) {
	    thr_put_bit(thr, bit, BIT4_X);
      } else {
	    thr_put_bit(thr, bit, word.value(use_index));
      }

      thr->words[index].w_int = use_index + 1;

      return true;
}

/*
 * %load/obj <var-label>
 */
bool of_LOAD_OBJ(vthread_t thr, vvp_code_t cp)
{
      vvp_net_t*net = cp->net;
      vvp_fun_signal_object*fun = dynamic_cast<vvp_fun_signal_object*> (net->fun);
      assert(fun);

      vvp_object_t val = fun->get_object();
      thr->push_object(val);

      return true;
}

/*
 * %load/real <var-label>
 */
bool of_LOAD_REAL(vthread_t thr, vvp_code_t cp)
{
      __vpiHandle*tmp = cp->handle;
      t_vpi_value val;

      val.format = vpiRealVal;
      vpi_get_value(tmp, &val);

      thr->push_real(val.value.real);

      return true;
}


bool of_LOAD_STR(vthread_t thr, vvp_code_t cp)
{
      vvp_net_t*net = cp->net;


      vvp_fun_signal_string*fun = dynamic_cast<vvp_fun_signal_string*> (net->fun);
      assert(fun);

      const string&val = fun->get_string();
      thr->push_str(val);

      return true;
}

bool of_LOAD_STRA(vthread_t thr, vvp_code_t cp)
{
      unsigned idx = cp->bit_idx[0];
      unsigned adr = thr->words[idx].w_int;
      string word;

	/* The result is 0.0 if the address is undefined. */
      if (thr_get_bit(thr, 4) == BIT4_1) {
	    word = "";
      } else {
	    word = array_get_word_str(cp->array, adr);
      }

      thr->push_str(word);
      return true;
}

/* %load/v <bit>, <label>, <wid>
 *
 * Implement the %load/v instruction. Load the vector value of the
 * requested width from the <label> functor starting in the thread bit
 * <bit>.
 *
 * The <bit> value is the destination in the thread vector store, and
 * is in cp->bit_idx[0].
 *
 * The <wid> value is the expected with of the vector, and is in
 * cp->bit_idx[1].
 *
 * The functor to read from is the vvp_net_t object pointed to by the
 * cp->net pointer.
 */
static void load_base(vvp_code_t cp, vvp_vector4_t&dst)
{
      vvp_net_t*net = cp->net;

	/* For the %load to work, the functor must actually be a
	   signal functor. Only signals save their vector value. */
      vvp_signal_value*sig = dynamic_cast<vvp_signal_value*> (net->fil);
      if (sig == 0) {
	    cerr << "%%load/v error: Net arg not a signal? "
		 << (net->fil ? typeid(*net->fil).name() : typeid(*net->fun).name()) << endl;
	    assert(sig);
      }

      sig->vec4_value(dst);
}

bool of_LOAD_VEC(vthread_t thr, vvp_code_t cp)
{
      unsigned bit = cp->bit_idx[0];
      unsigned wid = cp->bit_idx[1];

      vvp_vector4_t sig_value;
      load_base(cp, sig_value);

	/* Check the address once, before we scan the vector. */
      thr_check_addr(thr, bit+wid-1);

      if (sig_value.size() > wid)
	    sig_value.resize(wid);

	/* Copy the vector bits into the bits4 vector. Do the copy
	   directly to skip the excess calls to thr_check_addr. */
      thr->bits4.set_vec(bit, sig_value);

	/* If the source is shorter than the desired width, then pad
	   with BIT4_X values. */
      for (unsigned idx = sig_value.size() ; idx < wid ; idx += 1)
	    thr->bits4.set_bit(bit+idx, BIT4_X);

      return true;
}

/*
 * This is like of_LOAD_VEC, but includes an add of an integer value from
 * index 0. The <wid> is the expected result width not the vector width.
 */

bool of_LOAD_VP0(vthread_t thr, vvp_code_t cp)
{
      unsigned wid = cp->bit_idx[1];

        /* We need a vector this wide to make the math work correctly.
         * Copy the base bits into the vector, but keep the width. */
      vvp_vector4_t sig_value(wid, BIT4_0);

      vvp_vector4_t tmp;
      load_base(cp, tmp);
      sig_value.copy_bits(tmp);

      load_vp0_common(thr, cp, sig_value);
      return true;
}

bool of_LOAD_VP0_S(vthread_t thr, vvp_code_t cp)
{
      unsigned wid = cp->bit_idx[1];

      vvp_vector4_t tmp;
      load_base(cp, tmp);

        /* We need a vector this wide to make the math work correctly.
         * Copy the base bits into the vector, but keep the width. */
      vvp_vector4_t sig_value(wid, tmp.value(tmp.size()-1));
      sig_value.copy_bits(tmp);

      load_vp0_common(thr, cp, sig_value);
      return true;
}

/*
 * %load/x16 <bit>, <functor>, <wid>
 *
 * <bit> is the destination thread bit and must be >= 4.
 */
bool of_LOAD_X1P(vthread_t thr, vvp_code_t cp)
{
	// <bit> is the thread bit to load
      assert(cp->bit_idx[0] >= 4);
      unsigned bit = cp->bit_idx[0];
      int wid = cp->bit_idx[1];

	// <index> is the canonical base address of the part select.
      long index = thr->words[1].w_int;

	// <functor> is converted to a vvp_net_t pointer from which we
	// read our value.
      vvp_net_t*net = cp->net;

	// For the %load to work, the functor must actually be a
	// signal functor. Only signals save their vector value.
      vvp_signal_value*sig = dynamic_cast<vvp_signal_value*> (net->fil);
      assert(sig);

      for (long idx = 0 ; idx < wid ; idx += 1) {
	    long use_index = index + idx;
	    vvp_bit4_t val;
	    if (use_index < 0 || use_index >= (signed)sig->value_size())
		  val = BIT4_X;
	    else
		  val = sig->value(use_index);

	    thr_put_bit(thr, bit+idx, val);
      }

      return true;
}

static void do_verylong_mod(vthread_t thr, vvp_code_t cp,
			    bool left_is_neg, bool right_is_neg)
{
      bool out_is_neg = left_is_neg;
      int len=cp->number;
      unsigned char *a, *z, *t;
      a = new unsigned char[len+1];
      z = new unsigned char[len+1];
      t = new unsigned char[len+1];

      unsigned char carry;
      unsigned char temp;

      int mxa = -1, mxz = -1;
      int i;
      int current, copylen;

      unsigned idx1 = cp->bit_idx[0];
      unsigned idx2 = cp->bit_idx[1];

      unsigned lb_carry = left_is_neg? 1 : 0;
      unsigned rb_carry = right_is_neg? 1 : 0;
      for (unsigned idx = 0 ;  idx < cp->number ;  idx += 1) {
	    unsigned lb = thr_get_bit(thr, idx1);
	    unsigned rb = thr_get_bit(thr, idx2);

	    if ((lb | rb) & 2) {
		  delete []t;
		  delete []z;
		  delete []a;
		  goto x_out;
	    }

	    if (left_is_neg) {
		  lb = (1-lb) + lb_carry;
		  lb_carry = (lb & ~1)? 1 : 0;
		  lb &= 1;
	    }
	    if (right_is_neg) {
		  rb = (1-rb) + rb_carry;
		  rb_carry = (rb & ~1)? 1 : 0;
		  rb &= 1;
	    }

	    z[idx]=lb;
	    a[idx]=1-rb;	// for 2s complement add..

	    idx1 += 1;
	    if (idx2 >= 4)
		  idx2 += 1;
      }

      z[len]=0;
      a[len]=1;

      for(i=len-1;i>=0;i--) {
	    if(!a[i]) {
		  mxa=i;
		  break;
	    }
      }

      for(i=len-1;i>=0;i--) {
	    if(z[i]) {
		  mxz=i;
		  break;
	    }
      }

      if((mxa>mxz)||(mxa==-1)) {
	    if(mxa==-1) {
		  delete []t;
		  delete []z;
		  delete []a;
		  goto x_out;
	    }

	    goto tally;
      }

      copylen = mxa + 2;
      current = mxz - mxa;

      while(current > -1) {
	    carry = 1;
	    for(i=0;i<copylen;i++) {
		  temp = z[i+current] + a[i] + carry;
		  t[i] = (temp&1);
		  carry = (temp>>1);
	    }

	    if(carry) {
		  for(i=0;i<copylen;i++) {
			z[i+current] = t[i];
		  }
	    }

	    current--;
      }

 tally:

      carry = out_is_neg? 1 : 0;
      for (unsigned idx = 0 ;  idx < cp->number ;  idx += 1) {
	    unsigned ob = z[idx];
	    if (out_is_neg) {
		  ob = (1-ob) + carry;
		  carry = (ob & ~1)? 1 : 0;
		  ob = ob & 1;
	    }
	    thr_put_bit(thr, cp->bit_idx[0]+idx, ob?BIT4_1:BIT4_0);
      }

      delete []t;
      delete []z;
      delete []a;
      return;

 x_out:
      for (unsigned idx = 0 ;  idx < cp->number ;  idx += 1)
	    thr_put_bit(thr, cp->bit_idx[0]+idx, BIT4_X);

      return;
}

bool of_MAX_WR(vthread_t thr, vvp_code_t)
{
      double r = thr->pop_real();
      double l = thr->pop_real();
      if (r != r)
	    thr->push_real(l);
      else if (l != l)
	    thr->push_real(r);
      else if (r < l)
	    thr->push_real(l);
      else
	    thr->push_real(r);
      return true;
}

bool of_MIN_WR(vthread_t thr, vvp_code_t)
{
      double r = thr->pop_real();
      double l = thr->pop_real();
      if (r != r)
	    thr->push_real(l);
      else if (l != l)
	    thr->push_real(r);
      else if (r < l)
	    thr->push_real(r);
      else
	    thr->push_real(l);
      return true;
}

bool of_MOD(vthread_t thr, vvp_code_t cp)
{
      assert(cp->bit_idx[0] >= 4);

      if(cp->number <= 8*sizeof(unsigned long long)) {
	    unsigned idx1 = cp->bit_idx[0];
	    unsigned idx2 = cp->bit_idx[1];
	    unsigned long long lv = 0, rv = 0;

	    for (unsigned idx = 0 ;  idx < cp->number ;  idx += 1) {
		  unsigned long long lb = thr_get_bit(thr, idx1);
		  unsigned long long rb = thr_get_bit(thr, idx2);

		  if ((lb | rb) & 2)
			goto x_out;

		  lv |= (unsigned long long) lb << idx;
		  rv |= (unsigned long long) rb << idx;

		  idx1 += 1;
		  if (idx2 >= 4)
			idx2 += 1;
	    }

	    if (rv == 0)
		  goto x_out;

	    lv %= rv;

	    for (unsigned idx = 0 ;  idx < cp->number ;  idx += 1) {
		  thr_put_bit(thr, cp->bit_idx[0]+idx, (lv&1)?BIT4_1 : BIT4_0);
		  lv >>= 1;
	    }

	    return true;

      } else {
	    do_verylong_mod(thr, cp, false, false);
	    return true;
      }

 x_out:
      for (unsigned idx = 0 ;  idx < cp->number ;  idx += 1)
	    thr_put_bit(thr, cp->bit_idx[0]+idx, BIT4_X);

      return true;
}

bool of_MOD_S(vthread_t thr, vvp_code_t cp)
{
      assert(cp->bit_idx[0] >= 4);

	/* Handle the case that we can fit the bits into a long-long
	   variable. We cause use native % to do the work. */
      if(cp->number <= 8*sizeof(long long)) {
	    unsigned idx1 = cp->bit_idx[0];
	    unsigned idx2 = cp->bit_idx[1];
	    long long lv = 0, rv = 0;

	    for (unsigned idx = 0 ;  idx < cp->number ;  idx += 1) {
		  long long lb = thr_get_bit(thr, idx1);
		  long long rb = thr_get_bit(thr, idx2);

		  if ((lb | rb) & 2)
			goto x_out;

		  lv |= (long long) lb << idx;
		  rv |= (long long) rb << idx;

		  idx1 += 1;
		  if (idx2 >= 4)
			idx2 += 1;
	    }

	    if (rv == 0)
		  goto x_out;

	      /* Sign extend the signed operands when needed. */
	    if (cp->number < 8*sizeof(long long)) {
		  if (lv & (1LL << (cp->number-1)))
			lv |= -1LL << cp->number;
		  if (rv & (1LL << (cp->number-1)))
			rv |= -1LL << cp->number;
	    }

	    lv %= rv;

	    for (unsigned idx = 0 ;  idx < cp->number ;  idx += 1) {
		  thr_put_bit(thr, cp->bit_idx[0]+idx, (lv&1)?BIT4_1:BIT4_0);
		  lv >>= 1;
	    }

	    return true;

      } else {

	    bool left_is_neg
		  = thr_get_bit(thr,cp->bit_idx[0]+cp->number-1) == 1;
	    bool right_is_neg
		  = thr_get_bit(thr,cp->bit_idx[1]+cp->number-1) == 1;
	    do_verylong_mod(thr, cp, left_is_neg, right_is_neg);
	    return true;
      }

 x_out:
      for (unsigned idx = 0 ;  idx < cp->number ;  idx += 1)
	    thr_put_bit(thr, cp->bit_idx[0]+idx, BIT4_X);

      return true;
}

/*
 * %mod/wr
 */
bool of_MOD_WR(vthread_t thr, vvp_code_t)
{
      double r = thr->pop_real();
      double l = thr->pop_real();
      thr->push_real(fmod(l,r));

      return true;
}

/*
 * %mov <dest>, <src>, <wid>
 *   This instruction is implemented by the of_MOV function
 *   below. However, during runtime vvp might notice that the
 *   parameters have certain properties that make it possible to
 *   replace the of_MOV opcode with a more specific instruction that
 *   more directly does the job. All the of_MOV*_ functions are
 *   functions that of_MOV might use to replace itself.
 */

static bool of_MOV1XZ_(vthread_t thr, vvp_code_t cp)
{
      thr_check_addr(thr, cp->bit_idx[0]+cp->number-1);
      vvp_vector4_t tmp (cp->number, thr_index_to_bit4[cp->bit_idx[1]]);
      thr->bits4.set_vec(cp->bit_idx[0], tmp);
      return true;
}

static bool of_MOV_(vthread_t thr, vvp_code_t cp)
{
	/* This variant implements the general case that we know
	   neither the source nor the destination to be <4. Otherwise,
	   we copy all the bits manually. */

      thr_check_addr(thr, cp->bit_idx[0]+cp->number-1);
      thr_check_addr(thr, cp->bit_idx[1]+cp->number-1);

      thr->bits4.mov(cp->bit_idx[0], cp->bit_idx[1], cp->number);

      return true;
}

bool of_MOV(vthread_t thr, vvp_code_t cp)
{
      assert(cp->bit_idx[0] >= 4);

      if (cp->bit_idx[1] >= 4) {
	    cp->opcode = &of_MOV_;
	    return cp->opcode(thr, cp);

      } else {
	    cp->opcode = &of_MOV1XZ_;
	    return cp->opcode(thr, cp);
      }

      return true;
}

bool of_PAD(vthread_t thr, vvp_code_t cp)
{
      assert(cp->bit_idx[0] >= 4);

      vvp_bit4_t pad_bit;
      if (cp->bit_idx[1] < 4)
            pad_bit = thr_index_to_bit4[cp->bit_idx[1]];
      else
            pad_bit = thr->bits4.value(cp->bit_idx[1]);

      thr_check_addr(thr, cp->bit_idx[0]+cp->number-1);
      vvp_vector4_t tmp (cp->number, pad_bit);
      thr->bits4.set_vec(cp->bit_idx[0], tmp);
      return true;
}

/*
*  %mov/wu <dst>, <src>
*/
bool of_MOV_WU(vthread_t thr, vvp_code_t cp)
{
      unsigned dst = cp->bit_idx[0];
      unsigned src = cp->bit_idx[1];

      thr->words[dst].w_uint = thr->words[src].w_uint;
      return true;
}

bool of_MOVI(vthread_t thr, vvp_code_t cp)
{
      unsigned dst = cp->bit_idx[0];
      static unsigned long val[8] = {0, 0, 0, 0, 0, 0, 0, 0};
      unsigned wid = cp->number;

      thr_check_addr(thr, dst+wid-1);

      val[0] = cp->bit_idx[1];

      while (wid > 0) {
	    unsigned trans = wid;
	    if (trans > 8*CPU_WORD_BITS)
		  trans = 8*CPU_WORD_BITS;

	    thr->bits4.setarray(dst, trans, val);

	    val[0] = 0;
	    wid -= trans;
	    dst += trans;
      }

      return true;
}

bool of_MUL(vthread_t thr, vvp_code_t cp)
{
      unsigned adra = cp->bit_idx[0];
      unsigned adrb = cp->bit_idx[1];
      unsigned wid = cp->number;

      assert(adra >= 4);

      unsigned long*ap = vector_to_array(thr, adra, wid);
      if (ap == 0) {
	    vvp_vector4_t tmp(wid, BIT4_X);
	    thr->bits4.set_vec(adra, tmp);
	    return true;
      }

      unsigned long*bp = vector_to_array(thr, adrb, wid);
      if (bp == 0) {
	    delete[]ap;
	    vvp_vector4_t tmp(wid, BIT4_X);
	    thr->bits4.set_vec(adra, tmp);
	    return true;
      }

	// If the value fits in a single CPU word, then do it the easy way.
      if (wid <= CPU_WORD_BITS) {
	    ap[0] *= bp[0];
	    thr->bits4.setarray(adra, wid, ap);
	    delete[]ap;
	    delete[]bp;
	    return true;
      }

      unsigned words = (wid+CPU_WORD_BITS-1) / CPU_WORD_BITS;
      unsigned long*res = new unsigned long[words];
      for (unsigned idx = 0 ; idx < words ; idx += 1)
	    res[idx] = 0;

      for (unsigned mul_a = 0 ; mul_a < words ; mul_a += 1) {
	    for (unsigned mul_b = 0 ; mul_b < (words-mul_a) ; mul_b += 1) {
		  unsigned long sum;
		  unsigned long tmp = multiply_with_carry(ap[mul_a], bp[mul_b], sum);
		  unsigned base = mul_a + mul_b;
		  unsigned long carry = 0;
		  res[base] = add_with_carry(res[base], tmp, carry);
		  for (unsigned add_idx = base+1; add_idx < words; add_idx += 1) {
			res[add_idx] = add_with_carry(res[add_idx], sum, carry);
			sum = 0;
		  }
	    }
      }

      thr->bits4.setarray(adra, wid, res);
      delete[]ap;
      delete[]bp;
      delete[]res;
      return true;
}

bool of_MUL_WR(vthread_t thr, vvp_code_t)
{
      double r = thr->pop_real();
      double l = thr->pop_real();
      thr->push_real(l * r);

      return true;
}

bool of_MULI(vthread_t thr, vvp_code_t cp)
{
      unsigned adr = cp->bit_idx[0];
      unsigned long imm = cp->bit_idx[1];
      unsigned wid = cp->number;

      assert(adr >= 4);

      unsigned long*val = vector_to_array(thr, adr, wid);
	// If there are X bits in the value, then return X.
      if (val == 0) {
	    vvp_vector4_t tmp(cp->number, BIT4_X);
	    thr->bits4.set_vec(cp->bit_idx[0], tmp);
	    return true;
      }

	// If everything fits in a word, then do it the easy way.
      if (wid <= CPU_WORD_BITS) {
	    val[0] *= imm;
	    thr->bits4.setarray(adr, wid, val);
	    delete[]val;
	    return true;
      }

      unsigned words = (wid+CPU_WORD_BITS-1) / CPU_WORD_BITS;
      unsigned long*res = new unsigned long[words];

      multiply_array_imm(res, val, words, imm);

      thr->bits4.setarray(adr, wid, res);
      delete[]val;
      delete[]res;
      return true;
}

static bool of_NAND_wide(vthread_t thr, vvp_code_t cp)
{
      unsigned idx1 = cp->bit_idx[0];
      unsigned idx2 = cp->bit_idx[1];
      unsigned wid = cp->number;

      vvp_vector4_t val = vthread_bits_to_vector(thr, idx1, wid);
      val &= vthread_bits_to_vector(thr, idx2, wid);
      thr->bits4.set_vec(idx1, ~val);

      return true;
}

static bool of_NAND_narrow(vthread_t thr, vvp_code_t cp)
{
      unsigned idx1 = cp->bit_idx[0];
      unsigned idx2 = cp->bit_idx[1];
      unsigned wid = cp->number;

      for (unsigned idx = 0 ; idx < wid ; idx += 1) {
	    vvp_bit4_t lb = thr_get_bit(thr, idx1);
	    vvp_bit4_t rb = thr_get_bit(thr, idx2);
	    thr_put_bit(thr, idx1, ~(lb&rb));
	    idx1 += 1;
	    if (idx2 >= 4)
		  idx2 += 1;
      }

      return true;
}

bool of_NAND(vthread_t thr, vvp_code_t cp)
{
      assert(cp->bit_idx[0] >= 4);

      if (cp->number <= 4)
	    cp->opcode = &of_NAND_narrow;
      else
	    cp->opcode = &of_NAND_wide;

      return cp->opcode(thr, cp);
}

/*
 * %new/cobj <vpi_object>
 * This creates a new cobject (SystemVerilog class object) and pushes
 * it to the stack. The <vpi-object> is a __vpiHandle that is a
 * vpiClassDefn object that defines the item to be created.
 */
bool of_NEW_COBJ(vthread_t thr, vvp_code_t cp)
{
      const class_type*defn = dynamic_cast<const class_type*> (cp->handle);
      assert(defn);

      vvp_object_t tmp (new vvp_cobject(defn));
      thr->push_object(tmp);
      return true;
}

bool of_NEW_DARRAY(vthread_t thr, vvp_code_t cp)
{
      const char*text = cp->text;
      size_t size = thr->words[cp->bit_idx[0]].w_int;

      vvp_object_t obj;
      if (strcmp(text,"b8") == 0) {
	    obj = new vvp_darray_atom<uint8_t>(size);
      } else if (strcmp(text,"b16") == 0) {
	    obj = new vvp_darray_atom<uint16_t>(size);
      } else if (strcmp(text,"b32") == 0) {
	    obj = new vvp_darray_atom<uint32_t>(size);
      } else if (strcmp(text,"b64") == 0) {
	    obj = new vvp_darray_atom<uint64_t>(size);
      } else if (strcmp(text,"sb8") == 0) {
	    obj = new vvp_darray_atom<int8_t>(size);
      } else if (strcmp(text,"sb16") == 0) {
	    obj = new vvp_darray_atom<int16_t>(size);
      } else if (strcmp(text,"sb32") == 0) {
	    obj = new vvp_darray_atom<int32_t>(size);
      } else if (strcmp(text,"sb64") == 0) {
	    obj = new vvp_darray_atom<int64_t>(size);
      } else if (strcmp(text,"r") == 0) {
	    obj = new vvp_darray_real(size);
      } else if (strcmp(text,"S") == 0) {
	    obj = new vvp_darray_string(size);
      } else {
	    obj = new vvp_darray (size);
      }

      thr->push_object(obj);

      return true;
}

bool of_NOOP(vthread_t, vvp_code_t)
{
      return true;
}

bool of_NORR(vthread_t thr, vvp_code_t cp)
{
      assert(cp->bit_idx[0] >= 4);

      vvp_bit4_t lb = BIT4_1;
      unsigned idx2 = cp->bit_idx[1];

      for (unsigned idx = 0 ;  idx < cp->number ;  idx += 1) {

	    vvp_bit4_t rb = thr_get_bit(thr, idx2+idx);
	    if (rb == BIT4_1) {
		  lb = BIT4_0;
		  break;
	    }

	    if (rb != BIT4_0)
		  lb = BIT4_X;
      }

      thr_put_bit(thr, cp->bit_idx[0], lb);

      return true;
}

/*
 * Push a null to the object stack.
 */
bool of_NULL(vthread_t thr, vvp_code_t)
{
      vvp_object_t tmp;
      thr->push_object(tmp);
      return true;
}


bool of_ANDR(vthread_t thr, vvp_code_t cp)
{
      assert(cp->bit_idx[0] >= 4);

      vvp_bit4_t lb = BIT4_1;
      unsigned idx2 = cp->bit_idx[1];

      for (unsigned idx = 0 ;  idx < cp->number ;  idx += 1) {

	    vvp_bit4_t rb = thr_get_bit(thr, idx2+idx);
	    if (rb == BIT4_0) {
		  lb = BIT4_0;
		  break;
	    }

	    if (rb != BIT4_1)
		  lb = BIT4_X;
      }

      thr_put_bit(thr, cp->bit_idx[0], lb);

      return true;
}

bool of_NANDR(vthread_t thr, vvp_code_t cp)
{
      assert(cp->bit_idx[0] >= 4);

      vvp_bit4_t lb = BIT4_0;
      unsigned idx2 = cp->bit_idx[1];

      for (unsigned idx = 0 ;  idx < cp->number ;  idx += 1) {

	    vvp_bit4_t rb = thr_get_bit(thr, idx2+idx);
	    if (rb == BIT4_0) {
		  lb = BIT4_1;
		  break;
	    }

	    if (rb != BIT4_1)
		  lb = BIT4_X;
      }

      thr_put_bit(thr, cp->bit_idx[0], lb);

      return true;
}

bool of_ORR(vthread_t thr, vvp_code_t cp)
{
      assert(cp->bit_idx[0] >= 4);

      vvp_bit4_t lb = BIT4_0;
      unsigned idx2 = cp->bit_idx[1];

      for (unsigned idx = 0 ;  idx < cp->number ;  idx += 1) {

	    vvp_bit4_t rb = thr_get_bit(thr, idx2+idx);
	    if (rb == BIT4_1) {
		  lb = BIT4_1;
		  break;
	    }

	    if (rb != BIT4_0)
		  lb = BIT4_X;
      }

      thr_put_bit(thr, cp->bit_idx[0], lb);

      return true;
}

bool of_XORR(vthread_t thr, vvp_code_t cp)
{
      assert(cp->bit_idx[0] >= 4);

      vvp_bit4_t lb = BIT4_0;
      unsigned idx2 = cp->bit_idx[1];

      for (unsigned idx = 0 ;  idx < cp->number ;  idx += 1) {

	    vvp_bit4_t rb = thr_get_bit(thr, idx2+idx);
	    if (rb == BIT4_1)
		  lb = ~lb;
	    else if (rb != BIT4_0) {
		  lb = BIT4_X;
		  break;
	    }
      }

      thr_put_bit(thr, cp->bit_idx[0], lb);

      return true;
}

bool of_XNORR(vthread_t thr, vvp_code_t cp)
{
      assert(cp->bit_idx[0] >= 4);

      vvp_bit4_t lb = BIT4_1;
      unsigned idx2 = cp->bit_idx[1];

      for (unsigned idx = 0 ;  idx < cp->number ;  idx += 1) {

	    vvp_bit4_t rb = thr_get_bit(thr, idx2+idx);
	    if (rb == BIT4_1)
		  lb = ~lb;
	    else if (rb != BIT4_0) {
		  lb = BIT4_X;
		  break;
	    }
      }

      thr_put_bit(thr, cp->bit_idx[0], lb);

      return true;
}

static bool of_OR_wide(vthread_t thr, vvp_code_t cp)
{
      unsigned idx1 = cp->bit_idx[0];
      unsigned idx2 = cp->bit_idx[1];
      unsigned wid = cp->number;

      vvp_vector4_t val = vthread_bits_to_vector(thr, idx1, wid);
      val |= vthread_bits_to_vector(thr, idx2, wid);
      thr->bits4.set_vec(idx1, val);

      return true;
}

static bool of_OR_narrow(vthread_t thr, vvp_code_t cp)
{
      unsigned idx1 = cp->bit_idx[0];
      unsigned idx2 = cp->bit_idx[1];
      unsigned wid = cp->number;

      for (unsigned idx = 0 ; idx < wid ; idx += 1) {
	    vvp_bit4_t lb = thr_get_bit(thr, idx1);
	    vvp_bit4_t rb = thr_get_bit(thr, idx2);
	    thr_put_bit(thr, idx1, lb|rb);
	    idx1 += 1;
	    if (idx2 >= 4)
		  idx2 += 1;
      }

      return true;
}

bool of_OR(vthread_t thr, vvp_code_t cp)
{
      assert(cp->bit_idx[0] >= 4);

      if (cp->number <= 4)
	    cp->opcode = &of_OR_narrow;
      else
	    cp->opcode = &of_OR_wide;

      return cp->opcode(thr, cp);
}

static bool of_NOR_wide(vthread_t thr, vvp_code_t cp)
{
      assert(cp->bit_idx[0] >= 4);

      unsigned idx1 = cp->bit_idx[0];
      unsigned idx2 = cp->bit_idx[1];
      unsigned wid = cp->number;

      vvp_vector4_t val = vthread_bits_to_vector(thr, idx1, wid);
      val |= vthread_bits_to_vector(thr, idx2, wid);
      thr->bits4.set_vec(idx1, ~val);

      return true;
}

static bool of_NOR_narrow(vthread_t thr, vvp_code_t cp)
{
      unsigned idx1 = cp->bit_idx[0];
      unsigned idx2 = cp->bit_idx[1];
      unsigned wid = cp->number;

      for (unsigned idx = 0 ; idx < wid ; idx += 1) {
	    vvp_bit4_t lb = thr_get_bit(thr, idx1);
	    vvp_bit4_t rb = thr_get_bit(thr, idx2);
	    thr_put_bit(thr, idx1, ~(lb|rb));
	    idx1 += 1;
	    if (idx2 >= 4)
		  idx2 += 1;
      }

      return true;
}

bool of_NOR(vthread_t thr, vvp_code_t cp)
{
      assert(cp->bit_idx[0] >= 4);

      if (cp->number <= 4)
	    cp->opcode = &of_NOR_narrow;
      else
	    cp->opcode = &of_NOR_wide;

      return cp->opcode(thr, cp);
}

/*
 * %pop/obj <number>
 */
bool of_POP_OBJ(vthread_t thr, vvp_code_t cp)
{
      unsigned cnt = cp->number;
      thr->pop_object(cnt);
      return true;
}

/*
 * %pop/real <number>
 */
bool of_POP_REAL(vthread_t thr, vvp_code_t cp)
{
      unsigned cnt = cp->number;
      for (unsigned idx = 0 ; idx < cnt ; idx += 1) {
	    (void) thr->pop_real();
      }
      return true;
}

/*
 *  %pop/str <number>
 */
bool of_POP_STR(vthread_t thr, vvp_code_t cp)
{
      unsigned cnt = cp->number;
      thr->pop_str(cnt);
      return true;
}

bool of_POW(vthread_t thr, vvp_code_t cp)
{
      assert(cp->bit_idx[0] >= 4);

      unsigned idx = cp->bit_idx[0];
      unsigned idy = cp->bit_idx[1];
      unsigned wid = cp->number;
      vvp_vector2_t xv2 = vvp_vector2_t(vthread_bits_to_vector(thr, idx, wid));
      vvp_vector2_t yv2 = vvp_vector2_t(vthread_bits_to_vector(thr, idy, wid));

        /* If we have an X or Z in the arguments return X. */
      if (xv2.is_NaN() || yv2.is_NaN()) {
	    for (unsigned jdx = 0 ;  jdx < wid ;  jdx += 1)
		  thr_put_bit(thr, cp->bit_idx[0]+jdx, BIT4_X);
	    return true;
      }

        /* To make the result more manageable trim off the extra bits. */
      xv2.trim();
      yv2.trim();

      vvp_vector2_t result = pow(xv2, yv2);

        /* If the result is too small zero pad it. */
      if (result.size() < wid) {
	    for (unsigned jdx = wid-1;  jdx >= result.size();  jdx -= 1)
		  thr_put_bit(thr, cp->bit_idx[0]+jdx, BIT4_0);
	    wid = result.size();
      }

        /* Copy only what we need of the result. */
      for (unsigned jdx = 0;  jdx < wid;  jdx += 1)
	    thr_put_bit(thr, cp->bit_idx[0]+jdx,
	                result.value(jdx) ? BIT4_1 : BIT4_0);

      return true;
}

bool of_POW_S(vthread_t thr, vvp_code_t cp)
{
      assert(cp->bit_idx[0] >= 4);

      unsigned idx = cp->bit_idx[0];
      unsigned idy = cp->bit_idx[1];
      unsigned wid = cp->number;
      vvp_vector4_t xv = vthread_bits_to_vector(thr, idx, wid);
      vvp_vector4_t yv = vthread_bits_to_vector(thr, idy, wid);

        /* If we have an X or Z in the arguments return X. */
      if (xv.has_xz() || yv.has_xz()) {
	    for (unsigned jdx = 0 ;  jdx < wid ;  jdx += 1)
		  thr_put_bit(thr, cp->bit_idx[0]+jdx, BIT4_X);
	    return true;
      }

        /* Calculate the result using the double pow() function. */
      double xd, yd, resd;
      vector4_to_value(xv, xd, true);
      vector4_to_value(yv, yd, true);
	/* 2**-1 and -2**-1 are defined to be zero. */
      if ((yd == -1.0) && (fabs(xd) == 2.0)) resd = 0.0;
      else resd = pow(xd, yd);
      vvp_vector4_t res = vvp_vector4_t(wid, resd);

        /* Copy the result. */
      for (unsigned jdx = 0;  jdx < wid;  jdx += 1)
	    thr_put_bit(thr, cp->bit_idx[0]+jdx, res.value(jdx));

      return true;
}

bool of_POW_WR(vthread_t thr, vvp_code_t)
{
      double r = thr->pop_real();
      double l = thr->pop_real();
      thr->push_real(pow(l,r));

      return true;
}

/*
 * %prop/obj <pid>
 *
 * Load an object value from the cobject and push it onto the object stack.
 */
bool of_PROP_OBJ(vthread_t thr, vvp_code_t cp)
{
      unsigned pid = cp->number;

      vvp_object_t&obj = thr->peek_object();
      vvp_cobject*cobj = obj.peek<vvp_cobject>();

      vvp_object_t val;
      cobj->get_object(pid, val);

      thr->push_object(val);

      return true;
}

/*
 * %prop/r <pid>
 *
 * Load a real value from the cobject and push it onto the real value
 * stack.
 */
bool of_PROP_R(vthread_t thr, vvp_code_t cp)
{
      unsigned pid = cp->number;

      vvp_object_t&obj = thr->peek_object();
      vvp_cobject*cobj = obj.peek<vvp_cobject>();

      double val = cobj->get_real(pid);
      thr->push_real(val);

      return true;
}

/*
 * %prop/str <pid>
 *
 * Load a string value from the cobject and push it onto the real value
 * stack.
 */
bool of_PROP_STR(vthread_t thr, vvp_code_t cp)
{
      unsigned pid = cp->number;

      vvp_object_t&obj = thr->peek_object();
      vvp_cobject*cobj = obj.peek<vvp_cobject>();

      string val = cobj->get_string(pid);
      thr->push_str(val);

      return true;
}

/*
 * %prop/v <pid> <base> <wid>
 *
 * Load a property <id> from the cobject on the top of the stack into
 * the vector space at <base>.
 */
bool of_PROP_V(vthread_t thr, vvp_code_t cp)
{
      unsigned pid = cp->bit_idx[0];
      unsigned dst = cp->bit_idx[1];
      unsigned wid = cp->number;

      thr_check_addr(thr, dst+wid-1);
      vvp_object_t&obj = thr->peek_object();
      vvp_cobject*cobj = obj.peek<vvp_cobject>();

      vvp_vector4_t val;
      cobj->get_vec4(pid, val);

      if (val.size() > wid)
	    val.resize(wid);

      thr->bits4.set_vec(dst, val);

      if (val.size() < wid) {
	    for (unsigned idx = val.size() ; idx < wid ; idx += 1)
		  thr->bits4.set_bit(dst+idx, BIT4_X);
      }

      return true;
}

bool of_PUSHI_REAL(vthread_t thr, vvp_code_t cp)
{
      double mant = cp->bit_idx[0];
      uint32_t imant = cp->bit_idx[0];
      int exp = cp->bit_idx[1];

	// Detect +infinity
      if (exp==0x3fff && imant==0) {
	    thr->push_real(INFINITY);
	    return true;
      }
	// Detect -infinity
      if (exp==0x7fff && imant==0) {
	    thr->push_real(-INFINITY);
	    return true;
      }
	// Detect NaN
      if (exp==0x3fff) {
	    thr->push_real(nan(""));
	    return true;
      }

      double sign = (exp & 0x4000)? -1.0 : 1.0;

      exp &= 0x1fff;

      mant = sign * ldexp(mant, exp - 0x1000);
      thr->push_real(mant);
      return true;
}

bool of_PUSHI_STR(vthread_t thr, vvp_code_t cp)
{
      const char*text = cp->text;
      thr->push_str(string(text));
      return true;
}

bool of_PUSHV_STR(vthread_t thr, vvp_code_t cp)
{
      unsigned src = cp->bit_idx[0];
      unsigned wid = cp->bit_idx[1];

      vvp_vector4_t vec = vthread_bits_to_vector(thr, src, wid);
      size_t slen = (vec.size() + 7)/8;
      vector<char>buf;
      buf.reserve(slen);

      for (size_t idx = 0 ; idx < vec.size() ; idx += 8) {
	    char tmp = 0;
	    size_t trans = 8;
	    if (idx+trans > vec.size())
		  trans = vec.size() - idx;

	    for (size_t bdx = 0 ; bdx < trans ; bdx += 1) {
		  if (vec.value(idx+bdx) == BIT4_1)
			tmp |= 1 << bdx;
	    }

	    if (tmp != 0)
		  buf.push_back(tmp);
      }

      string val;
      for (vector<char>::reverse_iterator cur = buf.rbegin()
		 ; cur != buf.rend() ; ++cur) {
	    val.push_back(*cur);
      }

      thr->push_str(val);
      return true;
}

/*
 * %putc/str/v  <var>, <muxr>, <base>
 */
bool of_PUTC_STR_V(vthread_t thr, vvp_code_t cp)
{
      unsigned muxr = cp->bit_idx[0];
      unsigned base = cp->bit_idx[1];

	/* The mux is the index into the string. If it is <0, then
	   this operation cannot possible effect the string, so we are
	   done. */
      assert(muxr < 16);
      int32_t mux = thr->words[muxr].w_int;
      if (mux < 0)
	    return true;

	/* Extract the character from the vector space. If that byte
	   is null (8'hh00) then there is nothing more to do. */
      unsigned long*tmp = vector_to_array(thr, base, 8);
      if (tmp == 0)
	    return true;
      if (tmp[0] == 0)
	    return true;

      char tmp_val = tmp[0]&0xff;

	/* Get the existing value of the string. If we find that the
	   index is too big for the string, then give up. */
      vvp_net_t*net = cp->net;
      vvp_fun_signal_string*fun = dynamic_cast<vvp_fun_signal_string*> (net->fun);
      assert(fun);

      string val = fun->get_string();
      if (val.size() <= (size_t)mux)
	    return true;

	/* If the value to write is the same as the destination, then
	   stop now. */
      if (val[mux] == tmp_val)
	    return true;

	/* Finally, modify the string and write the new string to the
	   variable so that the new value propagates. */
      val[mux] = tmp_val;
      vvp_send_string(vvp_net_ptr_t(cp->net, 0), val, thr->wt_context);

      return true;
}

/*
 * These implement the %release/net and %release/reg instructions. The
 * %release/net instruction applies to a net kind of functor by
 * sending the release/net command to the command port. (See vvp_net.h
 * for details.) The %release/reg instruction is the same, but sends
 * the release/reg command instead. These are very similar to the
 * %deassign instruction.
 */
static bool do_release_vec(vvp_code_t cp, bool net_flag)
{
      vvp_net_t*net = cp->net;
      unsigned base  = cp->bit_idx[0];
      unsigned width = cp->bit_idx[1];

      assert(net->fil);

      if (base >= net->fil->filter_size()) return true;
      if (base+width > net->fil->filter_size())
	    width = net->fil->filter_size() - base;

      bool full_sig = base == 0 && width == net->fil->filter_size();

	// XXXX Can't really do this if this is a partial release?
      net->fil->force_unlink();

	/* Do we release all or part of the net? */
      vvp_net_ptr_t ptr (net, 0);
      if (full_sig) {
	    net->fil->release(ptr, net_flag);
      } else {
	    net->fil->release_pv(ptr, base, width, net_flag);
      }
      net->fun->force_flag();

      return true;
}

bool of_RELEASE_NET(vthread_t, vvp_code_t cp)
{
      return do_release_vec(cp, true);
}


bool of_RELEASE_REG(vthread_t, vvp_code_t cp)
{
      return do_release_vec(cp, false);
}

/* The type is 1 for registers and 0 for everything else. */
bool of_RELEASE_WR(vthread_t, vvp_code_t cp)
{
      vvp_net_t*net = cp->net;
      unsigned type  = cp->bit_idx[0];

      assert(net->fil);
      net->fil->force_unlink();

	// Send a command to this signal to unforce itself.
      vvp_net_ptr_t ptr (net, 0);
      net->fil->release(ptr, type==0);
      return true;
}

bool of_SCOPY(vthread_t thr, vvp_code_t)
{
      vvp_object_t tmp;
      thr->pop_object(tmp);

      vvp_object_t&dest = thr->peek_object();
      dest.shallow_copy(tmp);

      return true;
}

/*
 * This implements the "%set/av <label>, <bit>, <wid>" instruction. In
 * this case, the <label> is an array label, and the <bit> and <wid>
 * are the thread vector of a value to be written in.
 */
bool of_SET_AV(vthread_t thr, vvp_code_t cp)
{
      unsigned bit = cp->bit_idx[0];
      unsigned wid = cp->bit_idx[1];
      unsigned off = thr->words[1].w_int;
      unsigned adr = thr->words[3].w_int;

	/* Make a vector of the desired width. */
      vvp_vector4_t value = vthread_bits_to_vector(thr, bit, wid);

      array_set_word(cp->array, adr, off, value);
      return true;
}

/*
 * %set/dar  <label>, <bit>, <wid>
 */
bool of_SET_DAR(vthread_t thr, vvp_code_t cp)
{
      unsigned bit = cp->bit_idx[0];
      unsigned wid = cp->bit_idx[1];
      unsigned adr = thr->words[3].w_int;

	/* Make a vector of the desired width. */
      vvp_vector4_t value = vthread_bits_to_vector(thr, bit, wid);

      vvp_net_t*net = cp->net;
      vvp_fun_signal_object*obj = dynamic_cast<vvp_fun_signal_object*> (net->fun);
      assert(obj);

      vvp_darray*darray = obj->get_object().peek<vvp_darray>();
      assert(darray);

      darray->set_word(adr, value);
      return true;
}

/*
 * This implements the "%set/v <label>, <bit>, <wid>" instruction.
 *
 * The <label> is a reference to a vvp_net_t object, and it is in
 * cp->net.
 *
 * The <bit> is the thread bit address, and is in cp->bin_idx[0].
 *
 * The <wid> is the width of the vector I'm to make, and is in
 * cp->bin_idx[1].
 */
bool of_SET_VEC(vthread_t thr, vvp_code_t cp)
{
      assert(cp->bit_idx[1] > 0);
      unsigned bit = cp->bit_idx[0];
      unsigned wid = cp->bit_idx[1];

	/* set the value into port 0 of the destination. */
      vvp_net_ptr_t ptr (cp->net, 0);

      vvp_send_vec4(ptr, vthread_bits_to_vector(thr, bit, wid),
                    thr->wt_context);

      return true;
}


/*
 * Implement the %set/x instruction:
 *
 *      %set/x <functor>, <bit>, <wid>
 *
 * The bit value of a vector go into the addressed functor. Do not
 * transfer bits that are outside the signal range. Get the target
 * vector dimensions from the vvp_fun_signal addressed by the vvp_net
 * pointer.
 */
bool of_SET_X0(vthread_t thr, vvp_code_t cp)
{
      vvp_net_t*net = cp->net;
      unsigned bit = cp->bit_idx[0];
      unsigned wid = cp->bit_idx[1];

	// Implicitly, we get the base into the target vector from the
	// X0 register.
      long index = thr->words[0].w_int;

      vvp_signal_value*sig = dynamic_cast<vvp_signal_value*> (net->fil);
      assert(sig);

	// If the entire part is below the beginning of the vector,
	// then we are done.
      if (index < 0 && (wid <= (unsigned)-index))
	    return true;

	// If the entire part is above the end of the vector, then we
	// are done.
      if (index >= (long)sig->value_size())
	    return true;

	// If the part starts below the vector, then skip the first
	// few bits and reduce enough bits to start at the beginning
	// of the vector.
      if (index < 0) {
	    if (bit >= 4) bit += (unsigned) -index;
	    wid -= (unsigned) -index;
	    index = 0;
      }

	// Reduce the width to keep the part inside the vector.
      if (index+wid > sig->value_size())
	    wid = sig->value_size() - index;

      vvp_vector4_t bit_vec(wid);
      for (unsigned idx = 0 ;  idx < wid ;  idx += 1) {
	    vvp_bit4_t bit_val = thr_get_bit(thr, bit);
	    bit_vec.set_bit(idx, bit_val);
	    if (bit >= 4)
		  bit += 1;
      }

      vvp_net_ptr_t ptr (net, 0);
      vvp_send_vec4_pv(ptr, bit_vec, index, wid, sig->value_size(), thr->wt_context);

      return true;
}

bool of_SHIFTL_I0(vthread_t thr, vvp_code_t cp)
{
      int base = cp->bit_idx[0];
      int wid = cp->number;
      int shift = thr->words[0].w_int;

      assert(base >= 4);
      thr_check_addr(thr, base+wid-1);

      if (thr_get_bit(thr, 4) == BIT4_1) {
	    // The result is 'bx if the shift amount is undefined.
	    vvp_vector4_t tmp (wid, BIT4_X);
	    thr->bits4.set_vec(base, tmp);

      } else if (shift >= wid) {
	      // Shift is so far that all value is shifted out. Write
	      // in a constant 0 result.
	    vvp_vector4_t tmp (wid, BIT4_0);
	    thr->bits4.set_vec(base, tmp);

      } else if (shift > 0) {
	    vvp_vector4_t tmp (thr->bits4, base, wid-shift);
	    thr->bits4.set_vec(base+shift, tmp);

	      // Fill zeros on the bottom
	    vvp_vector4_t fil (shift, BIT4_0);
	    thr->bits4.set_vec(base, fil);

      } else if (shift <= -wid) {
	    vvp_vector4_t tmp (wid, BIT4_X);
	    thr->bits4.set_vec(base, tmp);

      } else if (shift < 0) {
	      // For a negative shift we pad with 'bx.
	    int idx;
	    for (idx = 0 ;  (idx-shift) < wid ;  idx += 1) {
		  unsigned src = base + idx - shift;
		  unsigned dst = base + idx;
		  thr_put_bit(thr, dst, thr_get_bit(thr, src));
	    }
	    for ( ;  idx < wid ;  idx += 1)
		  thr_put_bit(thr, base+idx, BIT4_X);
      }
      return true;
}

/*
 * This is an unsigned right shift:
 *
 *    %shiftr/i0 <bit>, <wid>
 *
 * The vector at address <bit> with width <wid> is shifted right a
 * number of bits stored in index/word register 0.
 */
bool of_SHIFTR_I0(vthread_t thr, vvp_code_t cp)
{
      int base = cp->bit_idx[0];
      int wid = cp->number;
      int shift = thr->words[0].w_int;

      assert(base >= 4);
      thr_check_addr(thr, base+wid-1);

      if (thr_get_bit(thr, 4) == BIT4_1) {
	      // The result is 'bx if the shift amount is undefined.
	    vvp_vector4_t tmp (wid, BIT4_X);
	    thr->bits4.set_vec(base, tmp);

      } else if (shift > wid) {
	      // Shift so far that the entire vector is shifted out.
	    vvp_vector4_t tmp (wid, BIT4_0);
	    thr->bits4.set_vec(base, tmp);

      } else if (shift > 0) {
	      // The mov method should handle overlapped source/dest
	    thr->bits4.mov(base, base+shift, wid-shift);

	    vvp_vector4_t tmp (shift, BIT4_0);
	    thr->bits4.set_vec(base+wid-shift, tmp);

      } else if (shift < -wid) {
	      // Negative shift is so far that all the value is shifted out.
	      // Write in a constant 'bx result.
	    vvp_vector4_t tmp (wid, BIT4_X);
	    thr->bits4.set_vec(base, tmp);

      } else if (shift < 0) {

	      // For a negative shift we pad with 'bx.
	    vvp_vector4_t tmp (thr->bits4, base, wid+shift);
	    thr->bits4.set_vec(base-shift, tmp);

	    vvp_vector4_t fil (-shift, BIT4_X);
	    thr->bits4.set_vec(base, fil);
      }
      return true;
}

bool of_SHIFTR_S_I0(vthread_t thr, vvp_code_t cp)
{
      int base = cp->bit_idx[0];
      int wid = cp->number;
      int shift = thr->words[0].w_int;
      vvp_bit4_t sign = thr_get_bit(thr, base+wid-1);

      if (thr_get_bit(thr, 4) == BIT4_1) {
	      // The result is 'bx if the shift amount is undefined.
	    vvp_vector4_t tmp (wid, BIT4_X);
	    thr->bits4.set_vec(base, tmp);
      } else if (shift >= wid) {
	    for (int idx = 0 ;  idx < wid ;  idx += 1)
		  thr_put_bit(thr, base+idx, sign);

      } else if (shift > 0) {
	    for (int idx = 0 ;  idx < (wid-shift) ;  idx += 1) {
		  unsigned src = base + idx + shift;
		  unsigned dst = base + idx;
		  thr_put_bit(thr, dst, thr_get_bit(thr, src));
	    }
	    for (int idx = (wid-shift) ;  idx < wid ;  idx += 1)
		  thr_put_bit(thr, base+idx, sign);

      } else if (shift < -wid) {
	      // Negative shift is so far that all the value is
	      // shifted out. Write in a constant 'bx result.
	    vvp_vector4_t tmp (wid, BIT4_X);
	    thr->bits4.set_vec(base, tmp);

      } else if (shift < 0) {

	      // For a negative shift we pad with 'bx.
	    vvp_vector4_t tmp (thr->bits4, base, wid+shift);
	    thr->bits4.set_vec(base-shift, tmp);

	    vvp_vector4_t fil (-shift, BIT4_X);
	    thr->bits4.set_vec(base, fil);
      }
      return true;
}

bool of_STORE_DAR_R(vthread_t thr, vvp_code_t cp)
{
      long adr = thr->words[3].w_int;

	// Pop the real value to be store...
      double value = thr->pop_real();

      vvp_net_t*net = cp->net;
      vvp_fun_signal_object*obj = dynamic_cast<vvp_fun_signal_object*> (net->fun);
      assert(obj);

      vvp_darray*darray = obj->get_object().peek<vvp_darray>();
      assert(darray);

      darray->set_word(adr, value);
      return true;
}

/*
 * %store/dar/str <var>
 * In this case, <var> is the name of a dynamic array. Signed index
 * register 3 contains the index into the dynamic array.
 */
bool of_STORE_DAR_STR(vthread_t thr, vvp_code_t cp)
{
      long adr = thr->words[3].w_int;

	// Pop the string to be stored...
      string value = thr->pop_str();

      vvp_net_t*net = cp->net;
      vvp_fun_signal_object*obj = dynamic_cast<vvp_fun_signal_object*> (net->fun);
      assert(obj);

      vvp_darray*darray = obj->get_object().peek<vvp_darray>();
      assert(darray);

      darray->set_word(adr, value);
      return true;
}


bool of_STORE_OBJ(vthread_t thr, vvp_code_t cp)
{
	/* set the value into port 0 of the destination. */
      vvp_net_ptr_t ptr (cp->net, 0);

      vvp_object_t val;
      thr->pop_object(val);

      vvp_send_object(ptr, val, thr->wt_context);

      return true;
}

/*
 * %store/prop/obj <id>
 *
 * Pop an object value from the object stack, and store the value into
 * the property of the object references by the top of the stack. Do NOT
 * pop the object stack.
 */
bool of_STORE_PROP_OBJ(vthread_t thr, vvp_code_t cp)
{
      size_t pid = cp->number;
      vvp_object_t val;
      thr->pop_object(val);

      vvp_object_t&obj = thr->peek_object();
      vvp_cobject*cobj = obj.peek<vvp_cobject>();
      assert(cobj);

      cobj->set_object(pid, val);

      return true;
}

/*
 * %store/prop/r <id>
 *
 * Pop a real value from the real stack, and store the value into the
 * property of the object references by the top of the stack. Do NOT
 * pop the object stack.
 */
bool of_STORE_PROP_R(vthread_t thr, vvp_code_t cp)
{
      size_t pid = cp->number;
      double val = thr->pop_real();

      vvp_object_t&obj = thr->peek_object();
      vvp_cobject*cobj = obj.peek<vvp_cobject>();
      assert(cobj);

      cobj->set_real(pid, val);

      return true;
}

/*
 * %store/prop/str <id>
 *
 * Pop a string value from the string stack, and store the value into
 * the property of the object references by the top of the stack. Do NOT
 * pop the object stack.
 */
bool of_STORE_PROP_STR(vthread_t thr, vvp_code_t cp)
{
      size_t pid = cp->number;
      string val = thr->pop_str();

      vvp_object_t&obj = thr->peek_object();
      vvp_cobject*cobj = obj.peek<vvp_cobject>();
      assert(cobj);

      cobj->set_string(pid, val);

      return true;
}

/*
 * %store/prop/v <id> <base> <wid>
 *
 * Store vector value into property <id> of cobject in the top of the stack.
 */
bool of_STORE_PROP_V(vthread_t thr, vvp_code_t cp)
{
      size_t pid = cp->bit_idx[0];
      unsigned src = cp->bit_idx[1];
      unsigned wid = cp->number;

      vvp_vector4_t val = vthread_bits_to_vector(thr, src, wid);

      vvp_object_t&obj = thr->peek_object();
      vvp_cobject*cobj = obj.peek<vvp_cobject>();
      assert(cobj);

      cobj->set_vec4(pid, val);
      return true;
}

bool of_STORE_REAL(vthread_t thr, vvp_code_t cp)
{
      double val = thr->pop_real();
	/* set the value into port 0 of the destination. */
      vvp_net_ptr_t ptr (cp->net, 0);
      vvp_send_real(ptr, val, thr->wt_context);

      return true;
}

/*
 * %store/reala <var-label> <index>
 */
bool of_STORE_REALA(vthread_t thr, vvp_code_t cp)
{
      unsigned idx = cp->bit_idx[0];
      unsigned adr = thr->words[idx].w_int;

      double val = thr->pop_real();
      array_set_word(cp->array, adr, val);

      return true;
}

bool of_STORE_STR(vthread_t thr, vvp_code_t cp)
{
	/* set the value into port 0 of the destination. */
      vvp_net_ptr_t ptr (cp->net, 0);

      string val = thr->pop_str();
      vvp_send_string(ptr, val, thr->wt_context);

      return true;
}

/*
 * %store/stra <array-label> <index>
 */
bool of_STORE_STRA(vthread_t thr, vvp_code_t cp)
{
      unsigned idx = cp->bit_idx[0];
      unsigned adr = thr->words[idx].w_int;

      string val = thr->pop_str();
      array_set_word(cp->array, adr, val);

      return true;
}


bool of_SUB(vthread_t thr, vvp_code_t cp)
{
      assert(cp->bit_idx[0] >= 4);

      unsigned long*lva = vector_to_array(thr, cp->bit_idx[0], cp->number);
      unsigned long*lvb = vector_to_array(thr, cp->bit_idx[1], cp->number);
      if (lva == 0 || lvb == 0)
	    goto x_out;


      unsigned long carry;
      carry = 1;
      for (unsigned idx = 0 ;  (idx*CPU_WORD_BITS) < cp->number ;  idx += 1)
	    lva[idx] = add_with_carry(lva[idx], ~lvb[idx], carry);


	/* We know from the vector_to_array that the address is valid
	   in the thr->bitr4 vector, so just do the set bit. */

      thr->bits4.setarray(cp->bit_idx[0], cp->number, lva);
      delete[]lva;
      delete[]lvb;

      return true;

 x_out:
      delete[]lva;
      delete[]lvb;

      vvp_vector4_t tmp(cp->number, BIT4_X);
      thr->bits4.set_vec(cp->bit_idx[0], tmp);

      return true;
}

bool of_SUB_WR(vthread_t thr, vvp_code_t)
{
      double r = thr->pop_real();
      double l = thr->pop_real();
      thr->push_real(l - r);
      return true;
}

bool of_SUBI(vthread_t thr, vvp_code_t cp)
{
      assert(cp->bit_idx[0] >= 4);

      unsigned word_count = (cp->number+CPU_WORD_BITS-1)/CPU_WORD_BITS;
      unsigned long imm = cp->bit_idx[1];
      unsigned long*lva = vector_to_array(thr, cp->bit_idx[0], cp->number);
      if (lva == 0)
	    goto x_out;


      unsigned long carry;
      carry = 1;
      for (unsigned idx = 0 ;  idx < word_count ;  idx += 1) {
	    lva[idx] = add_with_carry(lva[idx], ~imm, carry);
	    imm = 0UL;
      }

	/* We know from the vector_to_array that the address is valid
	   in the thr->bitr4 vector, so just do the set bit. */

      thr->bits4.setarray(cp->bit_idx[0], cp->number, lva);

      delete[]lva;

      return true;

 x_out:
      delete[]lva;

      vvp_vector4_t tmp(cp->number, BIT4_X);
      thr->bits4.set_vec(cp->bit_idx[0], tmp);

      return true;
}

/*
 * %substr <first>, <last>
 * Pop a string, take the substring (SystemVerilog style), and return
 * the result to the stack. This opcode actually works by editing the
 * string in place.
 */
bool of_SUBSTR(vthread_t thr, vvp_code_t cp)
{
      int32_t first = thr->words[cp->bit_idx[0]].w_int;
      int32_t last = thr->words[cp->bit_idx[1]].w_int;
      string&val = thr->peek_str(0);

      if (first < 0 || last < first || last >= (int32_t)val.size()) {
	    val = string("");
	    return true;
      }

      val = val.substr(first, last-first+1);
      return true;
}

/*
 * %substr/v <bitl>, <index>, <wid>
 */
bool of_SUBSTR_V(vthread_t thr, vvp_code_t cp)
{
      string&val = thr->peek_str(0);
      uint32_t bitl = cp->bit_idx[0];
      uint32_t sel = cp->bit_idx[1];
      unsigned wid = cp->number;

      thr_check_addr(thr, bitl+wid);
      assert(bitl >= 4);

      int32_t use_sel = thr->words[sel].w_int;

      vvp_vector4_t tmp (8);
      unsigned char_count = wid/8;
      for (unsigned idx = 0 ; idx < char_count ; idx += 1) {
	    unsigned long byte;
	    if (use_sel < 0)
		  byte = 0x00;
	    else if ((size_t)use_sel >= val.size())
		  byte = 0x00;
	    else
		  byte = val[use_sel];

	    thr->bits4.setarray(bitl, 8, &byte);
	    bitl += 8;
	    use_sel += 1;
      }

      return true;
}

bool of_FILE_LINE(vthread_t, vvp_code_t cp)
{
      if (show_file_line) {
	    vpiHandle handle = cp->handle;
	    cerr << vpi_get_str(vpiFile, handle) << ":"
	         << vpi_get(vpiLineNo, handle) << ": ";
	    cerr << vpi_get_str(_vpiDescription, handle);
	    cerr << endl;
      }
      return true;
}

/*
 * %test_nul <var-label>;
 */
bool of_TEST_NUL(vthread_t thr, vvp_code_t cp)
{
      vvp_net_t*net = cp->net;

      assert(net);
      vvp_fun_signal_object*obj = dynamic_cast<vvp_fun_signal_object*> (net->fun);
      assert(obj);

      if (obj->get_object().test_nil())
	    thr_put_bit(thr, 4, BIT4_1);
      else
	    thr_put_bit(thr, 4, BIT4_0);

      return true;
}

bool of_VPI_CALL(vthread_t thr, vvp_code_t cp)
{
      vpip_execute_vpi_call(thr, cp->handle);

      if (schedule_stopped()) {
	    if (! schedule_finished())
		  schedule_vthread(thr, 0, false);

	    return false;
      }

      return schedule_finished()? false : true;
}

/* %wait <label>;
 * Implement the wait by locating the vvp_net_T for the event, and
 * adding this thread to the threads list for the event. The some
 * argument is the  reference to the functor to wait for. This must be
 * an event object of some sort.
 */
bool of_WAIT(vthread_t thr, vvp_code_t cp)
{
      assert(! thr->waiting_for_event);
      thr->waiting_for_event = 1;

	/* Add this thread to the list in the event. */
      waitable_hooks_s*ep = dynamic_cast<waitable_hooks_s*> (cp->net->fun);
      assert(ep);
      thr->wait_next = ep->add_waiting_thread(thr);

	/* Return false to suspend this thread. */
      return false;
}


bool of_XNOR(vthread_t thr, vvp_code_t cp)
{
      assert(cp->bit_idx[0] >= 4);

      unsigned idx1 = cp->bit_idx[0];
      unsigned idx2 = cp->bit_idx[1];

      for (unsigned idx = 0 ;  idx < cp->number ;  idx += 1) {

	    vvp_bit4_t lb = thr_get_bit(thr, idx1);
	    vvp_bit4_t rb = thr_get_bit(thr, idx2);
	    thr_put_bit(thr, idx1, ~(lb ^ rb));

	    idx1 += 1;
	    if (idx2 >= 4)
		  idx2 += 1;
      }

      return true;
}


bool of_XOR(vthread_t thr, vvp_code_t cp)
{
      assert(cp->bit_idx[0] >= 4);

      unsigned idx1 = cp->bit_idx[0];
      unsigned idx2 = cp->bit_idx[1];

      for (unsigned idx = 0 ;  idx < cp->number ;  idx += 1) {

	    vvp_bit4_t lb = thr_get_bit(thr, idx1);
	    vvp_bit4_t rb = thr_get_bit(thr, idx2);

	    if ((lb == BIT4_1) && (rb == BIT4_1)) {
		  thr_put_bit(thr, idx1, BIT4_0);

	    } else if ((lb == BIT4_0) && (rb == BIT4_0)) {
		  thr_put_bit(thr, idx1, BIT4_0);

	    } else if ((lb == BIT4_1) && (rb == BIT4_0)) {
		  thr_put_bit(thr, idx1, BIT4_1);

	    } else if ((lb == BIT4_0) && (rb == BIT4_1)) {
		  thr_put_bit(thr, idx1, BIT4_1);

	    } else {
		  thr_put_bit(thr, idx1, BIT4_X);
	    }

	    idx1 += 1;
	    if (idx2 >= 4)
		  idx2 += 1;
      }

      return true;
}


bool of_ZOMBIE(vthread_t thr, vvp_code_t)
{
      thr->pc = codespace_null();
      if ((thr->parent == 0) && (thr->children.empty())) {
	    if (thr->delay_delete)
		  schedule_del_thr(thr);
	    else
		  vthread_delete(thr);
      }
      return false;
}

/*
 * This is a phantom opcode used to call user defined functions. It
 * is used in code generated by the .ufunc statement. It contains a
 * pointer to the executable code of the function and a pointer to
 * a ufunc_core object that has all the port information about the
 * function.
 */
bool of_EXEC_UFUNC(vthread_t thr, vvp_code_t cp)
{
      struct __vpiScope*child_scope = cp->ufunc_core_ptr->func_scope();
      assert(child_scope);

      assert(thr->children.empty());

        /* We can take a number of shortcuts because we know that a
           continuous assignment can only occur in a static scope. */
      assert(thr->wt_context == 0);
      assert(thr->rd_context == 0);

        /* If an automatic function, allocate a context for this call. */
      vvp_context_t child_context = 0;
      if (child_scope->is_automatic) {
            child_context = vthread_alloc_context(child_scope);
            thr->wt_context = child_context;
            thr->rd_context = child_context;
      }
	/* Copy all the inputs to the ufunc object to the port
	   variables of the function. This copies all the values
	   atomically. */
      cp->ufunc_core_ptr->assign_bits_to_ports(child_context);

	/* Create a temporary thread and run it immediately. A function
           may not contain any blocking statements, so vthread_run() can
           only return when the %end opcode is reached. */
      vthread_t child = vthread_new(cp->cptr, child_scope);
      child->wt_context = child_context;
      child->rd_context = child_context;
      child->is_scheduled = 1;
      vthread_run(child);
      running_thread = thr;

	/* Now copy the output from the result variable to the output
	   ports of the .ufunc device. */
      cp->ufunc_core_ptr->finish_thread();

        /* If an automatic function, free the context for this call. */
      if (child_scope->is_automatic) {
            vthread_free_context(child_context, child_scope);
            thr->wt_context = 0;
            thr->rd_context = 0;
      }

      return true;
}
