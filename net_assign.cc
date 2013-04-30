/*
 * Copyright (c) 2000-2011 Stephen Williams (steve@icarus.com)
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

# include "config.h"

# include  "netlist.h"
# include  "netclass.h"
# include  "netdarray.h"
# include  "ivl_assert.h"

/*
 * NetAssign
 */

unsigned count_lval_width(const NetAssign_*idx)
{
      unsigned wid = 0;
      while (idx) {
	    wid += idx->lwidth();
	    idx = idx->more;
      }
      return wid;
}

NetAssign_::NetAssign_(NetNet*s)
: sig_(s), word_(0), base_(0), sel_type_(IVL_SEL_OTHER)
{
      lwid_ = sig_->vector_width();
      sig_->incr_lref();
      more = 0;
      turn_sig_to_wire_on_release_ = false;
}

NetAssign_::~NetAssign_()
{
      if (sig_) {
	    sig_->decr_lref();
	    if (turn_sig_to_wire_on_release_ && sig_->peek_lref() == 0)
		  sig_->type(NetNet::WIRE);
      }

      assert( more == 0 );
      delete word_;
}

void NetAssign_::set_word(NetExpr*r)
{
      assert(word_ == 0);
      word_ = r;
}

NetExpr* NetAssign_::word()
{
      return word_;
}

const NetExpr* NetAssign_::word() const
{
      return word_;
}

const NetExpr* NetAssign_::get_base() const
{
      return base_;
}

ivl_select_type_t NetAssign_::select_type() const
{
      return sel_type_;
}

unsigned NetAssign_::lwidth() const
{
	// If the signal is a class type, then the situation is either
	// "a.b" or "a.b.<member>". If this is "a.b" (no
	// member/property reference, then return width==1. If this is
	// "a.b.<member>", then get the type of the <member> property
	// and return the width of that.
      if (const netclass_t*class_type = sig_->class_type()) {
	    if (member_.nil())
		  return 1;

	    const ivl_type_s*ptype = class_type->get_property(member_);
	    ivl_assert(*sig_, ptype);

	    return ptype->packed_width();
      }

      if (const netdarray_t*darray = sig_->darray_type()) {
	    if (word_ == 0)
		  return 1;
	    else
		  return darray->element_width();
      }

      return lwid_;
}

ivl_variable_type_t NetAssign_::expr_type() const
{
      if (const netclass_t*class_type = sig_->class_type()) {
	    if (member_.nil())
		  return sig_->data_type();

	    const ivl_type_s*tmp = class_type->get_property(member_);
	    return tmp->base_type();
      }

      if (const netdarray_t*darray = sig_->darray_type()) {
	    if (word_ == 0)
		  return IVL_VT_DARRAY;
	    else
		  return darray->element_base_type();
      }

      return sig_->data_type();
}

const ivl_type_s* NetAssign_::net_type() const
{
      if (const netclass_t*class_type = sig_->class_type()) {
	    if (member_.nil())
		  return sig_->net_type();

	    const ivl_type_s*tmp = class_type->get_property(member_);
	    ivl_assert(*sig_, tmp);
	    return tmp;
      }

      if (dynamic_cast<const netdarray_t*> (sig_->net_type())) {
	    if (word_ == 0)
		  return sig_->net_type();

	    return 0;
      }

      return 0;
}

const netenum_t*NetAssign_::enumeration() const
{
      const netenum_t*tmp = 0;

	// If the base signal is not an enumeration, return nil.
      if ( (tmp = sig_->enumeration()) == 0 )
	    return 0;

	// Part select of an enumeration is not an enumeration.
      if (base_ != 0)
	    return 0;

	// Concatenation of enumerations is not an enumeration.
      if (more != 0)
	    return 0;

      return tmp;
}

perm_string NetAssign_::name() const
{
      if (sig_) {
	    return sig_->name();
      } else {
	    return perm_string::literal("");
      }
}

NetNet* NetAssign_::sig() const
{
      return sig_;
}

void NetAssign_::set_part(NetExpr*base, unsigned wid,
                          ivl_select_type_t sel_type)
{
      base_ = base;
      lwid_ = wid;
      sel_type_ = sel_type;
}

void NetAssign_::set_property(const perm_string&mname)
{
      ivl_assert(*sig_, sig_->class_type());
      member_ = mname;
}

/*
 */
void NetAssign_::turn_sig_to_wire_on_release()
{
      turn_sig_to_wire_on_release_ = true;
}

NetAssignBase::NetAssignBase(NetAssign_*lv, NetExpr*rv)
: lval_(lv), rval_(rv), delay_(0)
{
}

NetAssignBase::~NetAssignBase()
{
      delete rval_;
      while (lval_) {
	    NetAssign_*tmp = lval_;
	    lval_ = tmp->more;
	    tmp->more = 0;
	    delete tmp;
      }
}

NetExpr* NetAssignBase::rval()
{
      return rval_;
}

const NetExpr* NetAssignBase::rval() const
{
      return rval_;
}

void NetAssignBase::set_rval(NetExpr*r)
{
      delete rval_;
      rval_ = r;
}

NetAssign_* NetAssignBase::l_val(unsigned idx)
{
      NetAssign_*cur = lval_;
      while (idx > 0) {
	    if (cur == 0)
		  return cur;

	    cur = cur->more;
	    idx -= 1;
      }

      assert(idx == 0);
      return cur;
}

const NetAssign_* NetAssignBase::l_val(unsigned idx) const
{
      const NetAssign_*cur = lval_;
      while (idx > 0) {
	    if (cur == 0)
		  return cur;

	    cur = cur->more;
	    idx -= 1;
      }

      assert(idx == 0);
      return cur;
}

unsigned NetAssignBase::l_val_count() const
{
      const NetAssign_*cur = lval_;
      unsigned cnt = 0;
      while (cur) {
	    cnt += 1;
	    cur = cur->more;
      }

      return cnt;
}

unsigned NetAssignBase::lwidth() const
{
      unsigned sum = 0;
      for (NetAssign_*cur = lval_ ;  cur ;  cur = cur->more)
	    sum += cur->lwidth();
      return sum;
}

void NetAssignBase::set_delay(NetExpr*expr)
{
      delay_ = expr;
}

const NetExpr* NetAssignBase::get_delay() const
{
      return delay_;
}

NetAssign::NetAssign(NetAssign_*lv, NetExpr*rv)
: NetAssignBase(lv, rv), op_(0)
{
}

NetAssign::NetAssign(NetAssign_*lv, char op, NetExpr*rv)
: NetAssignBase(lv, rv), op_(op)
{
}

NetAssign::~NetAssign()
{
}

NetAssignNB::NetAssignNB(NetAssign_*lv, NetExpr*rv, NetEvWait*ev, NetExpr*cnt)
: NetAssignBase(lv, rv)
{
      event_ = ev;
      count_ = cnt;
}

NetAssignNB::~NetAssignNB()
{
}

unsigned NetAssignNB::nevents() const
{
      if (event_) return event_->nevents();
      return 0;
}

const NetEvent*NetAssignNB::event(unsigned idx) const
{
      if (event_) return event_->event(idx);
      return 0;
}

const NetExpr*NetAssignNB::get_count() const
{
      return count_;
}

NetCAssign::NetCAssign(NetAssign_*lv, NetExpr*rv)
: NetAssignBase(lv, rv)
{
}

NetCAssign::~NetCAssign()
{
}

NetDeassign::NetDeassign(NetAssign_*l)
: NetAssignBase(l, 0)
{
}

NetDeassign::~NetDeassign()
{
}

NetForce::NetForce(NetAssign_*lv, NetExpr*rv)
: NetAssignBase(lv, rv)
{
}

NetForce::~NetForce()
{
}

NetRelease::NetRelease(NetAssign_*l)
: NetAssignBase(l, 0)
{
}

NetRelease::~NetRelease()
{
}
