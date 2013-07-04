/*
 * Copyright (c) 2013 Stephen Williams (steve@icarus.com)
 * Copyright CERN 2013 / Stephen Williams (steve@icarus.com)
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

# include  "subprogram.h"
# include  "entity.h"
# include  "vtype.h"
# include  "ivl_assert.h"

using namespace std;

Subprogram::Subprogram(perm_string nam, list<InterfacePort*>*ports,
		       const VType*return_type)
: name_(nam), parent_(0), ports_(ports), return_type_(return_type), statements_(0)
{
}

Subprogram::~Subprogram()
{
}

void Subprogram::set_parent(const ScopeBase*par)
{
      ivl_assert(*this, parent_ == 0);
      parent_ = par;
}

void Subprogram::set_program_body(list<SequentialStmt*>*stmt)
{
      ivl_assert(*this, statements_==0);
      statements_ = stmt;
}

bool Subprogram::compare_specification(Subprogram*that) const
{
      if (name_ != that->name_)
	    return false;

      if (return_type_==0) {
	    if (that->return_type_!=0)
		  return false;
      } else {
	    if (that->return_type_==0)
		  return false;

	    if (! return_type_->type_match(that->return_type_))
		  return false;
      }

      if (ports_==0) {
	    if (that->ports_!=0)
		  return false;

      } else {
	    if (that->ports_==0)
		  return false;

	    if (ports_->size() != that->ports_->size())
		  return false;
      }

      return true;
}

void Subprogram::write_to_stream(ostream&fd) const
{
      fd << "  function " << name_ << "(";
      if (ports_ && ! ports_->empty()) {
	    list<InterfacePort*>::const_iterator cur = ports_->begin();
	    InterfacePort*curp = *cur;
	    fd << curp->name << " : ";
	    curp->type->write_to_stream(fd);
	    for (++cur ; cur != ports_->end() ; ++cur) {
		  curp = *cur;
		  fd << "; " << curp->name << " : ";
		  curp->type->write_to_stream(fd);
	    }
      }
      fd << ") return ";
      return_type_->write_to_stream(fd);
      fd << ";" << endl;
}
