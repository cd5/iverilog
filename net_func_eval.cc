/*
 * Copyright (c) 2012-2013 Stephen Williams (steve@icarus.com)
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

# include  "netlist.h"
# include  "netmisc.h"
# include  "compiler.h"
# include  <typeinfo>
# include  "ivl_assert.h"

using namespace std;

/*
 * We only evaluate one function at a time, so to support the disable
 * statement, we just need to record the target block and then early
 * terminate each enclosing block or loop statement until we get back
 * to the target block.
 */
static const NetScope*disable = 0;

static NetExpr* fix_assign_value(const NetNet*lhs, NetExpr*rhs)
{
      NetEConst*ce = dynamic_cast<NetEConst*>(rhs);
      if (ce == 0) return rhs;

      unsigned lhs_width = lhs->vector_width();
      unsigned rhs_width = rhs->expr_width();
      if (rhs_width < lhs_width) {
            rhs = pad_to_width(rhs, lhs_width, *rhs);
      } else if (rhs_width > lhs_width) {
            verinum value(ce->value(), lhs_width);
            ce = new NetEConst(value);
            ce->set_line(*rhs);
            delete rhs;
	    rhs = ce;
      }
      rhs->cast_signed(lhs->get_signed());
      return rhs;
}

NetExpr* NetFuncDef::evaluate_function(const LineInfo&loc, const std::vector<NetExpr*>&args) const
{
	// Make the context map;
      map<perm_string,NetExpr*>::iterator ptr;
      map<perm_string,NetExpr*>context_map;

      if (debug_eval_tree) {
	    cerr << loc.get_fileline() << ": debug: "
		 << "Evaluate function " << scope_->basename() << endl;
      }

	// Put the return value into the map...
      context_map[scope_->basename()] = 0;

	// Load the input ports into the map...
      ivl_assert(loc, ports_.size() == args.size());
      for (size_t idx = 0 ; idx < ports_.size() ; idx += 1) {
	    perm_string aname = ports_[idx]->name();
	    context_map[aname] = fix_assign_value(ports_[idx], args[idx]);

	    if (debug_eval_tree) {
		  cerr << loc.get_fileline() << ": debug: "
		       << "   input " << aname << " = " << *args[idx] << endl;
	    }
      }

	// Ask the scope to collect definitions for local values. This
	// fills in the context_map with local variables held by the scope.
      scope_->evaluate_function_find_locals(loc, context_map);

	// Perform the evaluation. Note that if there were errors
	// when compiling the function definition, we may not have
	// a valid statement.
      bool flag = statement_ && statement_->evaluate_function(loc, context_map);

	// Extract the result...
      ptr = context_map.find(scope_->basename());
      NetExpr*res = ptr->second;
      context_map.erase(ptr);


	// Cleanup the rest of the context.
      for (ptr = context_map.begin() ; ptr != context_map.end() ; ++ptr) {
	    delete ptr->second;
      }

	// Done.
      if (flag)
	    return res;

      delete res;
      return 0;
}

void NetScope::evaluate_function_find_locals(const LineInfo&loc,
			         map<perm_string,NetExpr*>&context_map) const
{
      for (map<perm_string,NetNet*>::const_iterator cur = signals_map_.begin()
		 ; cur != signals_map_.end() ; ++cur) {

	    const NetNet*tmp = cur->second;
	      // Skip ports, which are handled elsewhere.
	    if (tmp->port_type() != NetNet::NOT_A_PORT)
		  continue;

	    context_map[tmp->name()] = 0;

	    if (debug_eval_tree) {
		  cerr << loc.get_fileline() << ": debug: "
		       << "   (local) " << tmp->name() << endl;
	    }
      }
}

NetExpr* NetExpr::evaluate_function(const LineInfo&,
				    map<perm_string,NetExpr*>&) const
{
      cerr << get_fileline() << ": sorry: I don't know how to evaluate this expression at compile time." << endl;
      cerr << get_fileline() << ":      : Expression type:" << typeid(*this).name() << endl;

      return 0;
}

bool NetProc::evaluate_function(const LineInfo&,
				map<perm_string,NetExpr*>&) const
{
      cerr << get_fileline() << ": sorry: I don't know how to evaluate this statement at compile time." << endl;
      cerr << get_fileline() << ":      : Statement type:" << typeid(*this).name() << endl;

      return false;
}

bool NetAssign::evaluate_function(const LineInfo&loc,
				  map<perm_string,NetExpr*>&context_map) const
{
      if (l_val_count() != 1) {
	    cerr << get_fileline() << ": sorry: I don't know how to evaluate "
		  "concatenated l-values here." << endl;
	    return false;
      }

      const NetAssign_*lval = l_val(0);

      map<perm_string,NetExpr*>::iterator ptr = context_map.find(lval->name());
      ivl_assert(*this, ptr != context_map.end());

	// Do not support having l-values that are unpacked arrays.
      ivl_assert(loc, lval->word() == 0);

	// Evaluate the r-value expression.
      NetExpr*rval_result = rval()->evaluate_function(loc, context_map);
      if (rval_result == 0)
	    return false;

      if (const NetExpr*base_expr = lval->get_base()) {
	    NetExpr*base_result = base_expr->evaluate_function(loc, context_map);
	    if (base_result == 0) {
		  delete rval_result;
		  return false;
	    }

	    NetEConst*base_const = dynamic_cast<NetEConst*>(base_result);
	    ivl_assert(loc, base_const);

	    long base = base_const->value().as_long();

	    list<long>prefix (0);
	    base = lval->sig()->sb_to_idx(prefix, base);

	    if (ptr->second == 0)
		  ptr->second = make_const_x(lval->sig()->vector_width());

	    ivl_assert(loc, base + lval->lwidth() <= ptr->second->expr_width());

	    NetEConst*ptr_const = dynamic_cast<NetEConst*>(ptr->second);
	    verinum lval_v = ptr_const->value();
	    NetEConst*rval_const = dynamic_cast<NetEConst*>(rval_result);
	    verinum rval_v = cast_to_width(rval_const->value(), lval->lwidth());

	    for (unsigned idx = 0 ; idx < rval_v.len() ; idx += 1)
		  lval_v.set(idx+base, rval_v[idx]);

	    delete base_result;
	    delete rval_result;
	    rval_result = new NetEConst(lval_v);
      } else {
	    rval_result = fix_assign_value(lval->sig(), rval_result);
      }

      if (ptr->second)
	    delete ptr->second;

      if (debug_eval_tree) {
	    cerr << get_fileline() << ": debug: "
		 << "NetAssign::evaluate_function: " << lval->name()
		 << " = " << *rval_result << endl;
      }

      ptr->second = rval_result;

      return true;
}

/*
 * Evaluating a NetBlock in a function is a simple matter of
 * evaluating the statements in order.
 */
bool NetBlock::evaluate_function(const LineInfo&loc,
				 map<perm_string,NetExpr*>&context_map) const
{
      bool flag = true;
      NetProc*cur = last_;
      do {
	    cur = cur->next_;
	    bool cur_flag = cur->evaluate_function(loc, context_map);
	    flag = flag && cur_flag;
      } while (cur != last_ && !disable);

      if (disable == subscope_) disable = 0;

      return flag;
}

bool NetCase::evaluate_function_vect_(const LineInfo&loc,
				map<perm_string,NetExpr*>&context_map) const
{
      NetExpr*case_expr = expr_->evaluate_function(loc, context_map);
      if (case_expr == 0)
	    return false;

      NetEConst*case_const = dynamic_cast<NetEConst*> (case_expr);
      ivl_assert(loc, case_const);

      verinum case_val = case_const->value();
      delete case_expr;

      NetProc*default_statement = 0;

      for (unsigned cnt = 0 ; cnt < nitems_ ; cnt += 1) {
            Item*item = &items_[cnt];

            if (item->guard == 0) {
                  default_statement = item->statement;
                  continue;
            }

            NetExpr*item_expr = item->guard->evaluate_function(loc, context_map);
            if (item_expr == 0)
                  return false;

            NetEConst*item_const = dynamic_cast<NetEConst*> (item_expr);
            ivl_assert(loc, item_const);

            verinum item_val = item_const->value();
            delete item_expr;

            ivl_assert(loc, item_val.len() == case_val.len());

            bool match = true;
            for (unsigned idx = 0 ; idx < item_val.len() ; idx += 1) {
                  verinum::V bit_a = case_val.get(idx);
                  verinum::V bit_b = item_val.get(idx);

                  if (bit_a == verinum::Vx && type_ == EQX) continue;
                  if (bit_b == verinum::Vx && type_ == EQX) continue;

                  if (bit_a == verinum::Vz && type_ != EQ) continue;
                  if (bit_b == verinum::Vz && type_ != EQ) continue;

                  if (bit_a != bit_b) {
                        match = false;
                        break;
                  }
            }
            if (!match) continue;

            return item->statement->evaluate_function(loc, context_map);
      }

      if (default_statement)
            return default_statement->evaluate_function(loc, context_map);

      return true;
}

bool NetCase::evaluate_function_real_(const LineInfo&loc,
				map<perm_string,NetExpr*>&context_map) const
{
      NetExpr*case_expr = expr_->evaluate_function(loc, context_map);
      if (case_expr == 0)
	    return false;

      NetECReal*case_const = dynamic_cast<NetECReal*> (case_expr);
      ivl_assert(loc, case_const);

      double case_val = case_const->value().as_double();
      delete case_expr;

      NetProc*default_statement = 0;

      for (unsigned cnt = 0 ; cnt < nitems_ ; cnt += 1) {
            Item*item = &items_[cnt];

            if (item->guard == 0) {
                  default_statement = item->statement;
                  continue;
            }

            NetExpr*item_expr = item->guard->evaluate_function(loc, context_map);
            if (item_expr == 0)
                  return false;

            NetECReal*item_const = dynamic_cast<NetECReal*> (item_expr);
            ivl_assert(loc, item_const);

            double item_val = item_const->value().as_double();
            delete item_expr;

            if (item_val != case_val) continue;

            return item->statement->evaluate_function(loc, context_map);
      }

      if (default_statement)
            return default_statement->evaluate_function(loc, context_map);

      return true;
}

bool NetCase::evaluate_function(const LineInfo&loc,
				map<perm_string,NetExpr*>&context_map) const
{
      if (expr_->expr_type() == IVL_VT_REAL)
	    return evaluate_function_real_(loc, context_map);
      else
	    return evaluate_function_vect_(loc, context_map);
}

bool NetCondit::evaluate_function(const LineInfo&loc,
				  map<perm_string,NetExpr*>&context_map) const
{
      NetExpr*cond = expr_->evaluate_function(loc, context_map);
      if (cond == 0)
	    return false;

      NetEConst*cond_const = dynamic_cast<NetEConst*> (cond);
      ivl_assert(loc, cond_const);

      long val = cond_const->value().as_long();
      delete cond;

      if (val)
	      // The condition is true, so evaluate the if clause
	    return (if_ == 0) || if_->evaluate_function(loc, context_map);
      else
	      // The condition is false, so evaluate the else clause
	    return (else_ == 0) || else_->evaluate_function(loc, context_map);
}

bool NetDisable::evaluate_function(const LineInfo&,
				   map<perm_string,NetExpr*>&) const
{
      disable = target_;
      return true;
}

bool NetForever::evaluate_function(const LineInfo&loc,
				   map<perm_string,NetExpr*>&context_map) const
{
      bool flag = true;

      if (debug_eval_tree) {
	    cerr << get_fileline() << ": debug: NetForever::evaluate_function: "
		 << "Start loop" << endl;
      }

      while (flag && !disable) {
	    flag = flag && statement_->evaluate_function(loc, context_map);
      }

      if (debug_eval_tree) {
	    cerr << get_fileline() << ": debug: NetForever::evaluate_function: "
		 << "Done loop" << endl;
      }

      return flag;
}

bool NetRepeat::evaluate_function(const LineInfo&loc,
				  map<perm_string,NetExpr*>&context_map) const
{
      bool flag = true;

	// Evaluate the condition expression to try and get the
	// condition for the loop.
      NetExpr*count_expr = expr_->evaluate_function(loc, context_map);
      if (count_expr == 0) return false;

      NetEConst*count_const = dynamic_cast<NetEConst*> (count_expr);
      ivl_assert(loc, count_const);

      long count = count_const->value().as_long();
      delete count_expr;

      if (debug_eval_tree) {
	    cerr << get_fileline() << ": debug: NetRepeat::evaluate_function: "
		 << "Repeating " << count << " times." << endl;
      }

      while ((count > 0) && flag && !disable) {
	    flag = flag && statement_->evaluate_function(loc, context_map);
	    count -= 1;
      }

      if (debug_eval_tree) {
	    cerr << get_fileline() << ": debug: NetRepeat::evaluate_function: "
		 << "Finished loop" << endl;
      }

      return flag;
}

bool NetSTask::evaluate_function(const LineInfo&,
				 map<perm_string,NetExpr*>&) const
{
	// system tasks within a constant function are ignored
      return true;
}

bool NetWhile::evaluate_function(const LineInfo&loc,
				 map<perm_string,NetExpr*>&context_map) const
{
      bool flag = true;

      if (debug_eval_tree) {
	    cerr << get_fileline() << ": debug: NetWhile::evaluate_function: "
		 << "Start loop" << endl;
      }

      while (flag && !disable) {
	      // Evaluate the condition expression to try and get the
	      // condition for the loop.
	    NetExpr*cond = cond_->evaluate_function(loc, context_map);
	    if (cond == 0) {
		  flag = false;
		  break;
	    }

	    NetEConst*cond_const = dynamic_cast<NetEConst*> (cond);
	    ivl_assert(loc, cond_const);

	    long val = cond_const->value().as_long();
	    delete cond;

	      // If the condition is false, then break.
	    if (val == 0)
		  break;

	      // The condition is true, so evaluate the statement
	      // another time.
	    bool tmp_flag = proc_->evaluate_function(loc, context_map);
	    if (! tmp_flag)
		  flag = false;
      }

      if (debug_eval_tree) {
	    cerr << get_fileline() << ": debug: NetWhile::evaluate_function: "
		 << "Done loop" << endl;
      }

      return flag;
}

NetExpr* NetEBinary::evaluate_function(const LineInfo&loc,
				    map<perm_string,NetExpr*>&context_map) const
{
      NetExpr*lval = left_->evaluate_function(loc, context_map);
      NetExpr*rval = right_->evaluate_function(loc, context_map);

      if (lval == 0 || rval == 0) {
	    delete lval;
	    delete rval;
	    return 0;
      }

      NetExpr*res = eval_arguments_(lval, rval);
      delete lval;
      delete rval;
      return res;
}

NetExpr* NetEConcat::evaluate_function(const LineInfo&loc,
				    map<perm_string,NetExpr*>&context_map) const
{
      vector<NetExpr*>vals(parms_.size());
      unsigned gap = 0;

      unsigned valid_vals = 0;
      for (unsigned idx = 0 ;  idx < parms_.size() ;  idx += 1) {
            ivl_assert(*this, parms_[idx]);
            vals[idx] = parms_[idx]->evaluate_function(loc, context_map);
            if (vals[idx] == 0) continue;

            gap += vals[idx]->expr_width();

            valid_vals += 1;
      }

      NetExpr*res = 0;
      if (valid_vals == parms_.size()) {
            res = eval_arguments_(vals, gap);
      }
      for (unsigned idx = 0 ;  idx < vals.size() ;  idx += 1) {
            delete vals[idx];
      }
      return res;
}

NetExpr* NetEConst::evaluate_function(const LineInfo&,
				      map<perm_string,NetExpr*>&) const
{
      NetEConst*res = new NetEConst(value_);
      res->set_line(*this);
      return res;
}

NetExpr* NetECReal::evaluate_function(const LineInfo&,
				      map<perm_string,NetExpr*>&) const
{
      NetECReal*res = new NetECReal(value_);
      res->set_line(*this);
      return res;
}

NetExpr* NetESelect::evaluate_function(const LineInfo&loc,
				    map<perm_string,NetExpr*>&context_map) const
{
      NetExpr*sub_exp = expr_->evaluate_function(loc, context_map);
      ivl_assert(loc, sub_exp);

      NetEConst*sub_const = dynamic_cast<NetEConst*> (sub_exp);
      ivl_assert(loc, sub_exp);

      verinum sub = sub_const->value();
      delete sub_exp;

      long base = 0;
      if (base_) {
	    NetExpr*base_val = base_->evaluate_function(loc, context_map);
	    ivl_assert(loc, base_val);

	    NetEConst*base_const = dynamic_cast<NetEConst*>(base_val);
	    ivl_assert(loc, base_const);

	    base = base_const->value().as_long();
	    delete base_val;
      } else {
	    sub.has_sign(has_sign());
	    sub = pad_to_width(sub, expr_width());
      }

      verinum res (verinum::Vx, expr_width());
      for (unsigned idx = 0 ; idx < res.len() ; idx += 1)
	    res.set(idx, sub[base+idx]);

      NetEConst*res_const = new NetEConst(res);
      return res_const;
}

NetExpr* NetESignal::evaluate_function(const LineInfo&,
				       map<perm_string,NetExpr*>&context_map) const
{
      if (word_) {
	    cerr << get_fileline() << ": sorry: I don't know how to evaluate signal word selects at compile time." << endl;
	    return 0;
      }

      map<perm_string,NetExpr*>::iterator ptr = context_map.find(name());
      if (ptr == context_map.end()) {
	    cerr << get_fileline() << ": error: Cannot evaluate " << name()
		 << " in this context." << endl;
	    return 0;
      }

      if (ptr->second == 0) {
	    switch (expr_type()) {
		case IVL_VT_REAL:
		  ptr->second = new NetECReal( verireal(0.0) );
		  break;
		case IVL_VT_BOOL:
		  ptr->second = make_const_0(expr_width());
		  break;
		case IVL_VT_LOGIC:
		  ptr->second = make_const_x(expr_width());
		  break;
		default:
		  cerr << get_fileline() << ": sorry: I don't know how to initialize " << *this << endl;
		  return 0;
	    }
      }

      return ptr->second->dup_expr();
}

NetExpr* NetETernary::evaluate_function(const LineInfo&loc,
				    map<perm_string,NetExpr*>&context_map) const
{
      auto_ptr<NetExpr> cval (cond_->evaluate_function(loc, context_map));

      switch (const_logical(cval.get())) {

	  case C_0:
	    return false_val_->evaluate_function(loc, context_map);
	  case C_1:
	    return true_val_->evaluate_function(loc, context_map);
	  case C_X:
	    break;
	  default:
	    cerr << get_fileline() << ": error: Condition expression is not constant here." << endl;
	    return 0;
      }

      NetExpr*tval = true_val_->evaluate_function(loc, context_map);
      NetExpr*fval = false_val_->evaluate_function(loc, context_map);

      NetExpr*res = blended_arguments_(tval, fval);
      delete tval;
      delete fval;
      return res;
}

NetExpr* NetEUnary::evaluate_function(const LineInfo&loc,
				    map<perm_string,NetExpr*>&context_map) const
{
      NetExpr*val = expr_->evaluate_function(loc, context_map);
      if (val == 0) return 0;

      NetExpr*res = eval_arguments_(val);
      delete val;
      return res;
}

NetExpr* NetESFunc::evaluate_function(const LineInfo&loc,
				      map<perm_string,NetExpr*>&context_map) const
{
      ID id = built_in_id_();
      ivl_assert(*this, id != NOT_BUILT_IN);

      NetExpr*val0 = 0;
      NetExpr*val1 = 0;
      NetExpr*res = 0;
      switch (nargs_(id)) {
	  case 1:
	    val0 = parms_[0]->evaluate_function(loc, context_map);
	    if (val0 == 0) break;
	    res = evaluate_one_arg_(id, val0);
	    break;
	  case 2:
	    val0 = parms_[0]->evaluate_function(loc, context_map);
	    val1 = parms_[1]->evaluate_function(loc, context_map);
	    if (val0 == 0 || val1 == 0) break;
	    res = evaluate_two_arg_(id, val0, val1);
	    break;
	  default:
	    ivl_assert(*this, 0);
	    break;
      }
      delete val0;
      delete val1;
      return res;
}

NetExpr* NetEUFunc::evaluate_function(const LineInfo&loc,
				      map<perm_string,NetExpr*>&context_map) const
{
      NetFuncDef*def = func_->func_def();
      ivl_assert(*this, def);

      vector<NetExpr*>args(parms_.size());
      for (unsigned idx = 0 ;  idx < parms_.size() ;  idx += 1)
	    args[idx] = parms_[idx]->evaluate_function(loc, context_map);

      NetExpr*res = def->evaluate_function(*this, args);
      return res;
}
