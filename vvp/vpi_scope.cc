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

# include  "compile.h"
# include  "vpi_priv.h"
# include  "symbols.h"
# include  "statistics.h"
# include  "config.h"
#ifdef CHECK_WITH_VALGRIND
# include  "vvp_cleanup.h"
#endif
# include  <cstring>
# include  <cstdlib>
# include  <cassert>
# include  "ivl_alloc.h"


static vpiHandle *vpip_root_table_ptr = 0;
static unsigned   vpip_root_table_cnt = 0;

vpiHandle vpip_make_root_iterator(void)
{
      assert(vpip_root_table_ptr);
      assert(vpip_root_table_cnt);
      return vpip_make_iterator(vpip_root_table_cnt,
				vpip_root_table_ptr, false);
}

void vpip_make_root_iterator(__vpiHandle**&table, unsigned&ntable)
{
      table = vpip_root_table_ptr;
      ntable = vpip_root_table_cnt;
}

#ifdef CHECK_WITH_VALGRIND
void port_delete(__vpiHandle*handle);

static void delete_sub_scopes(struct __vpiScope *scope)
{
      for (unsigned idx = 0; idx < scope->nintern; idx += 1) {
	    struct __vpiScope*lscope = static_cast<__vpiScope*>
	          ((scope->intern)[idx]);
	    switch(scope->intern[idx]->get_type_code()) {
		case vpiFunction:
		case vpiTask:
		  contexts_delete(lscope);
		case vpiModule:
		case vpiNamedBegin:
		case vpiNamedFork:
		  delete_sub_scopes(lscope);
		  vthreads_delete(lscope);
		  delete (scope->intern)[idx];
		  break;
		case vpiMemory:
		case vpiNetArray:
		  memory_delete((scope->intern)[idx]);
		  break;
		case vpiModPath:
		    /* The destination ModPath is cleaned up later. */
		  delete (scope->intern)[idx];
		  break;
		case vpiNamedEvent:
		  named_event_delete((scope->intern)[idx]);
		  break;
		case vpiNet:
		case vpiReg:
		case vpiIntegerVar:
		case vpiLongIntVar:
		case vpiShortIntVar:
		case vpiIntVar:
		case vpiByteVar:
		case vpiBitVar:
		  signal_delete((scope->intern)[idx]);
		  break;
		case vpiParameter:
		  parameter_delete((scope->intern)[idx]);
		  break;
		case vpiRealVar:
		  real_delete((scope->intern)[idx]);
		  break;
		case vpiEnumTypespec:
		  enum_delete((scope->intern)[idx]);
		  break;
		case vpiPort:
		  port_delete((scope->intern)[idx]);
		  break;
		case vpiStringVar:
		  string_delete((scope->intern)[idx]);
		  break;
		case vpiClassVar:
		  class_delete((scope->intern)[idx]);
		  break;
		case vpiRegArray:
		  darray_delete((scope->intern)[idx]);
		  break;
		default:
		  fprintf(stderr, "Need support for type: %d\n",
		          scope->intern[idx]->get_type_code());
		  assert(0);
		  break;
	    }
      }
      free(scope->intern);

	/* Clean up any class definitions. */
      map<std::string, class_type*>::iterator citer;
      for (citer = scope->classes.begin();
           citer != scope->classes.end(); ++ citer ) {
            class_def_delete(citer->second);
      }
}

void root_table_delete(void)
{
      for (unsigned idx = 0; idx < vpip_root_table_cnt; idx += 1) {
	    struct __vpiScope *scope = static_cast<__vpiScope *>
	          (vpip_root_table_ptr[idx]);
	    delete_sub_scopes(scope);
	    vthreads_delete(scope);
	    delete scope;
      }
      free(vpip_root_table_ptr);
      vpip_root_table_ptr = 0;
      vpip_root_table_cnt = 0;
}
#endif

static int scope_get(int code, vpiHandle obj)
{
      struct __vpiScope*ref = dynamic_cast<__vpiScope*>(obj);
      assert(obj);

      switch (code) {
	  case vpiCellInstance:
	    return (int) ref->is_cell;

	  case vpiDefLineNo:
	    return ref->def_lineno;

	  case vpiLineNo:
	    return ref->lineno;

	  case vpiTimeUnit:
	    return ref->time_units;

	  case vpiTimePrecision:
	    return ref->time_precision;

	  case vpiTopModule:
	    return 0x0 == ref->scope;

          case vpiAutomatic:
	    return (int) ref->is_automatic;
      }

      return vpiUndefined;
}

static void construct_scope_fullname(struct __vpiScope*ref, char*buf)
{
      if (ref->scope) {
	    construct_scope_fullname(ref->scope, buf);
	    strcat(buf, ".");
      }

      strcat(buf, ref->name);
}

static const char* scope_get_type(int code)
{
      switch (code) {
          case vpiModule:
            return "vpiModule";
          case vpiFunction:
            return "vpiFunction";
          case vpiTask:
            return "vpiTask";
          case vpiNamedBegin:
            return "vpiNamedBegin";
          case vpiNamedFork:
            return "vpiNamedFork";
          default:
            fprintf(stderr, "VPI error: invalid scope type code %d.\n", code);
            return NULL;
      }
}

static char* scope_get_str(int code, vpiHandle obj)
{
      struct __vpiScope*ref = dynamic_cast<__vpiScope*>(obj);
      assert(ref);

      char buf[4096];  // XXX is a fixed buffer size really reliable?
      const char *p=0;
      switch (code) {
	  case vpiDefFile:
	    p = file_names[ref->def_file_idx];
	    break;

	  case vpiFile:
	    p = file_names[ref->file_idx];
	    break;

	  case vpiFullName:
	    buf[0] = 0;
	    construct_scope_fullname(ref, buf);
	    p = buf;
	    break;

	  case vpiName:
	    p = ref->name;
	    break;

	  case vpiDefName:
	    p = ref->tname;
	    break;

	  case vpiType:
	    p = scope_get_type(code);
	    break;

	  default:
	    fprintf(stderr, "VPI error: invalid scope string code %d.\n", code);
	    return NULL;
      }
      return simple_set_rbuf_str(p);
}

static vpiHandle scope_get_handle(int code, vpiHandle obj)
{
      struct __vpiScope*rfp = dynamic_cast<__vpiScope*>(obj);
      assert(rfp);

      switch (code) {

	  case vpiScope:
	    return rfp->scope;

	  case vpiModule:
	    return rfp->scope;
      }

      return 0;
}

/* compares vpiType's considering object classes */
static int compare_types(int code, int type)
{
	/* NOTE: The Verilog VPI does not for any object support
	   vpiScope as an iterator parameter, so it is used here as a
	   means to scan everything in the *current* scope. */
      if (code == vpiScope)
	    return 1;

      if (code == type)
	    return 1;

      if ( code == vpiInternalScope &&
	     (type == vpiModule ||
	     type == vpiFunction ||
	     type == vpiTask ||
	     type == vpiNamedBegin ||
	     type == vpiNamedFork) )
	    return 1;

      if ( code == vpiVariables &&
	     (type == vpiIntegerVar  ||
	      type == vpiBitVar      ||
	      type == vpiByteVar     ||
	      type == vpiShortIntVar ||
	      type == vpiIntVar      ||
	      type == vpiLongIntVar  ||
	      type == vpiTimeVar     ||
	      type == vpiRealVar))
	    return 1;

      return 0;
}

static vpiHandle module_iter_subset(int code, struct __vpiScope*ref)
{
      unsigned mcnt = 0, ncnt = 0;
      vpiHandle*args;

      for (unsigned idx = 0 ;  idx < ref->nintern ;  idx += 1)
	    if (compare_types(code, ref->intern[idx]->get_type_code()))
		  mcnt += 1;

      if (mcnt == 0)
	    return 0;

      args = (vpiHandle*)calloc(mcnt, sizeof(vpiHandle));
      for (unsigned idx = 0 ;  idx < ref->nintern ;  idx += 1)
	    if (compare_types(code, ref->intern[idx]->get_type_code()))
		  args[ncnt++] = ref->intern[idx];

      assert(ncnt == mcnt);

      return vpip_make_iterator(mcnt, args, true);
}

/*
 * This function implements the vpi_iterate method for vpiModule and
 * similar objects. The vpi_iterate allows the user to iterate over
 * things that are contained in the scope object, by generating an
 * iterator for the requested set of items.
 */
static vpiHandle module_iter(int code, vpiHandle obj)
{
      struct __vpiScope*ref = dynamic_cast<__vpiScope*>(obj);
      assert(ref);

      return module_iter_subset(code, ref);
}


int __vpiScope::vpi_get(int code)
{ return scope_get(code, this); }

char*__vpiScope::vpi_get_str(int code)
{ return scope_get_str(code, this); }

vpiHandle __vpiScope::vpi_handle(int code)
{ return scope_get_handle(code, this); }

vpiHandle __vpiScope::vpi_iterate(int code)
{ return module_iter(code, this); }


struct vpiScopeModule  : public __vpiScope {
      inline vpiScopeModule() { }
      int get_type_code(void) const { return vpiModule; }
};

struct vpiScopePackage  : public __vpiScope {
      inline vpiScopePackage() { }
      int get_type_code(void) const { return vpiPackage; }
};

struct vpiScopeTask  : public __vpiScope {
      inline vpiScopeTask() { }
      int get_type_code(void) const { return vpiTask; }
};

struct vpiScopeFunction  : public __vpiScope {
      inline vpiScopeFunction() { }
      int get_type_code(void) const { return vpiFunction; }
};

struct vpiScopeBegin  : public __vpiScope {
      inline vpiScopeBegin() { }
      int get_type_code(void) const { return vpiNamedBegin; }
};

struct vpiScopeFork  : public __vpiScope {
      inline vpiScopeFork() { }
      int get_type_code(void) const { return vpiNamedFork; }
};

struct vpiScopeClass  : public __vpiScope {
      inline vpiScopeClass() { }
      int get_type_code(void) const { return vpiClassTypespec; }
};

/*
 * The current_scope is a compile time concept. As the vvp source is
 * compiled, items that have scope are placed in the current
 * scope. The ".scope" directives select the scope that is current.
 */
static struct __vpiScope*current_scope = 0;

void vpip_attach_to_scope(struct __vpiScope*scope, vpiHandle obj)
{
      assert(scope);
      unsigned idx = scope->nintern++;

      if (scope->intern == 0)
	    scope->intern = (vpiHandle*)
		  malloc(sizeof(vpiHandle));
      else
	    scope->intern = (vpiHandle*)
		  realloc(scope->intern, sizeof(vpiHandle)*scope->nintern);

      scope->intern[idx] = obj;
}

/*
 * When the compiler encounters a scope declaration, this function
 * creates and initializes a __vpiScope object with the requested name
 * and within the addressed parent. The label is used as a key in the
 * symbol table and the name is used to construct the actual object.
 */
void
compile_scope_decl(char*label, char*type, char*name, char*tname,
                   char*parent, long file_idx, long lineno,
                   long def_file_idx, long def_lineno, long is_cell)
{
      count_vpi_scopes += 1;

      char*base_type;
      bool is_automatic;
      if (strncmp(type,"auto",4) == 0) {
	    is_automatic = true;
            base_type = &type[4];
      } else {
	    is_automatic = false;
            base_type = &type[0];
      }

      struct __vpiScope*scope;
      if (strcmp(base_type,"module") == 0) {
	    scope = new vpiScopeModule;
      } else if (strcmp(base_type,"function") == 0) {
	    scope = new vpiScopeFunction;
      } else if (strcmp(base_type,"task") == 0) {
	    scope = new vpiScopeTask;
      } else if (strcmp(base_type,"fork") == 0) {
	    scope = new vpiScopeFork;
      } else if (strcmp(base_type,"begin") == 0) {
	    scope = new vpiScopeBegin;
      } else if (strcmp(base_type,"generate") == 0) {
	    scope = new vpiScopeBegin;
      } else if (strcmp(base_type,"package") == 0) {
	    scope = new vpiScopePackage;
      } else if (strcmp(base_type,"class") == 0) {
	    scope = new vpiScopeClass;
      } else {
	    scope = new vpiScopeModule;
	    assert(0);
      }

      scope->name = vpip_name_string(name);
      if (tname) scope->tname = vpip_name_string(tname);
      else scope->tname = vpip_name_string("");
      scope->file_idx = (unsigned) file_idx;
      scope->lineno  = (unsigned) lineno;
      scope->def_file_idx = (unsigned) def_file_idx;
      scope->def_lineno  = (unsigned) def_lineno;
      scope->is_automatic = is_automatic;
      scope->intern = 0;
      scope->nintern = 0;
      scope->item = 0;
      scope->nitem = 0;
      scope->live_contexts = 0;
      scope->free_contexts = 0;

      if (is_cell) scope->is_cell = true;
      else scope->is_cell = false;

      current_scope = scope;

      compile_vpi_symbol(label, scope);

      free(label);
      free(type);
      delete[] name;
      delete[] tname;

      if (parent) {
	    static vpiHandle obj;
	    compile_vpi_lookup(&obj, parent);
	    assert(obj);
	    struct __vpiScope*sp = dynamic_cast<__vpiScope*>(obj);
	    vpip_attach_to_scope(sp, scope);
	    scope->scope = dynamic_cast<__vpiScope*>(obj);

	      /* Inherit time units and precision from the parent scope. */
	    scope->time_units = sp->time_units;
	    scope->time_precision = sp->time_precision;

      } else {
	    scope->scope = 0x0;

	    unsigned cnt = vpip_root_table_cnt + 1;
	    vpip_root_table_ptr = (vpiHandle*)
		  realloc(vpip_root_table_ptr, cnt * sizeof(vpiHandle));
	    vpip_root_table_ptr[vpip_root_table_cnt] = scope;
	    vpip_root_table_cnt = cnt;

	      /* Root scopes inherit time_units and precision from the
	         system precision. */
	    scope->time_units = vpip_get_time_precision();
	    scope->time_precision = vpip_get_time_precision();
      }
}

void compile_scope_recall(char*symbol)
{
	/* A __vpiScope starts with a __vpiHandle structure so this is
	   a safe cast. We need the (void*) to avoid a dereferenced
	   type punned pointer warning from some gcc compilers. */
      compile_vpi_lookup((vpiHandle*)(void*)&current_scope, symbol);
      assert(current_scope);
}

/*
 * This function handles the ".timescale" directive in the vvp
 * source. It sets in the current scope the specified units value.
 */
void compile_timescale(long units, long precision)
{
      assert(current_scope);
      current_scope->time_units = units;
      current_scope->time_precision = precision;
}

struct __vpiScope* vpip_peek_current_scope(void)
{
      return current_scope;
}

void vpip_attach_to_current_scope(vpiHandle obj)
{
      vpip_attach_to_scope(current_scope, obj);
}

struct __vpiScope* vpip_peek_context_scope(void)
{
      struct __vpiScope*scope = current_scope;

        /* A context is allocated for each automatic task or function.
           Storage for nested scopes (named blocks) is allocated in
           the parent context. */
      while (scope->scope && scope->scope->is_automatic)
            scope = scope->scope;

      return scope;
}

unsigned vpip_add_item_to_context(automatic_hooks_s*item,
                                  struct __vpiScope*scope)
{
      assert(scope);
      assert(scope->is_automatic);

      unsigned idx = scope->nitem++;

      if (scope->item == 0)
	    scope->item = (automatic_hooks_s**)
		  malloc(sizeof(automatic_hooks_s*));
      else
	    scope->item = (automatic_hooks_s**)
		  realloc(scope->item, sizeof(automatic_hooks_s*)*scope->nitem);

      scope->item[idx] = item;

        /* Offset the context index by 2 to leave space for the list links. */
      return 2 + idx;
}


class vpiPortInfo  : public __vpiHandle {
    public:
      vpiPortInfo( __vpiScope *parent,
                    unsigned index,
                    int vpi_direction,
                    unsigned width,
                    const char *name );
      ~vpiPortInfo();

      int get_type_code(void) const { return vpiPort; }

      int vpi_get(int code);
      char* vpi_get_str(int code);
      vpiHandle vpi_handle(int code);

    private:
      __vpiScope *parent_;
      unsigned  index_;
      int       direction_;
      unsigned  width_;
      const char *name_;
};

vpiPortInfo::vpiPortInfo( __vpiScope *parent,
              unsigned index,
              int vpi_direction,
              unsigned width,
              const char *name ) :
      parent_(parent),
      index_(index),
      direction_(vpi_direction),
      width_(width),
      name_(name)
{
}

vpiPortInfo::~vpiPortInfo()
{
      delete[] name_;
}

#ifdef CHECK_WITH_VALGRIND
void port_delete(__vpiHandle *handle)
{
      delete dynamic_cast<vpiPortInfo *>(handle);
}
#endif

int vpiPortInfo::vpi_get(int code)
{
      switch( code ) {

        case vpiDirection :
          return direction_;
        case vpiPortIndex :
          return index_;
        case vpiSize :
          return width_;
        default :
          return vpiUndefined;
      }

}


char *vpiPortInfo::vpi_get_str(int code)
{
      switch( code ) {
        case vpiName :
          return simple_set_rbuf_str(name_);
        default :
          return NULL;
      }

}


vpiHandle vpiPortInfo::vpi_handle(int code)
{

      switch (code) {

          case vpiParent:
          case vpiScope:
          case vpiModule:
            return parent_;
          default :
            break;
      }

      return 0;
}


/* Port info is meta-data to allow vpi queries of the port signature of modules for
 * code-generators etc.  There are no actual nets corresponding to instances of module ports
 * as elaboration directly connects nets connected through module ports.
 */
void compile_port_info( unsigned index, int vpi_direction, unsigned width, const char *name )
{
    vpiHandle obj = new vpiPortInfo( vpip_peek_current_scope(),
                                     index, vpi_direction, width, name );
    vpip_attach_to_current_scope(obj);
}
