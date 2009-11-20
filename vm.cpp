/*
 * MacRuby VM.
 *
 * This file is covered by the Ruby license. See COPYING for more details.
 * 
 * Copyright (C) 2008-2009, Apple Inc. All rights reserved.
 */

#define ROXOR_VM_DEBUG		0
#define ROXOR_COMPILER_DEBUG 	0

#include <llvm/Module.h>
#include <llvm/DerivedTypes.h>
#include <llvm/Constants.h>
#include <llvm/CallingConv.h>
#include <llvm/Instructions.h>
#include <llvm/ModuleProvider.h>
#include <llvm/PassManager.h>
#include <llvm/Analysis/Verifier.h>
#include <llvm/Target/TargetData.h>
#include <llvm/ExecutionEngine/JIT.h>
#include <llvm/ExecutionEngine/JITMemoryManager.h>
#include <llvm/ExecutionEngine/GenericValue.h>
#include <llvm/Target/TargetData.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/Target/TargetSelect.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Intrinsics.h>
#include <llvm/Bitcode/ReaderWriter.h>
using namespace llvm;

#if ROXOR_COMPILER_DEBUG
# include <mach/mach.h>
# include <mach/mach_time.h>
#endif

#include "ruby/ruby.h"
#include "ruby/node.h"
#include "id.h"
#include "vm.h"
#include "compiler.h"
#include "objc.h"
#include "dtrace.h"

#include <objc/objc-exception.h>
#include <cxxabi.h>
using namespace __cxxabiv1;

#include <execinfo.h>
#include <dlfcn.h>

#include <iostream>
#include <fstream>

RoxorCore *RoxorCore::shared = NULL;
RoxorVM *RoxorVM::main = NULL;
pthread_key_t RoxorVM::vm_thread_key;

VALUE rb_cTopLevel = 0;

struct RoxorFunction {
    Function *f;
    RoxorScope *scope;
    unsigned char *start;
    unsigned char *end;
    void *imp;
    std::vector<unsigned char *> ehs;

    RoxorFunction(Function *_f, RoxorScope *_scope, unsigned char *_start,
	    unsigned char *_end) {
	f = _f;
	scope = _scope;
	start = _start;
	end = _end;
	imp = NULL; 	// lazy
    }
};

class RoxorJITManager : public JITMemoryManager {
    private:
        JITMemoryManager *mm;
	std::vector<struct RoxorFunction *> functions;

    public:
	RoxorJITManager() : JITMemoryManager() { 
	    mm = CreateDefaultMemManager(); 
	}

	struct RoxorFunction *find_function(uint8_t *addr) {
	     if (functions.empty()) {
		return NULL;
	     }
	     // TODO optimize me!
	     RoxorFunction *front = functions.front();
	     RoxorFunction *back = functions.back();
	     if (addr < front->start || addr > back->end) {
		return NULL;
	     }
	     std::vector<struct RoxorFunction *>::iterator iter = 
		 functions.begin();
	     while (iter != functions.end()) {
		RoxorFunction *f = *iter;
		if (addr >= f->start && addr <= f->end) {
		    return f;
		}
		++iter;
	     }
	     return NULL;
	}

	RoxorFunction *delete_function(Function *func) {
	    std::vector<struct RoxorFunction *>::iterator iter = 
		functions.begin();
	    while (iter != functions.end()) {
		RoxorFunction *f = *iter;
		if (f->f == func) {
		    functions.erase(iter);
		    return f;
		}
		++iter;
	    }
	    return NULL;
	}

	void setMemoryWritable(void) { 
	    mm->setMemoryWritable(); 
	}

	void setMemoryExecutable(void) { 
	    mm->setMemoryExecutable(); 
	}

	uint8_t *allocateSpace(intptr_t Size, unsigned Alignment) { 
	    return mm->allocateSpace(Size, Alignment); 
	}

	uint8_t *allocateGlobal(uintptr_t Size, unsigned Alignment) {
	    return mm->allocateGlobal(Size, Alignment);
	}

	void AllocateGOT(void) {
	    mm->AllocateGOT();
	}

	uint8_t *getGOTBase() const {
	    return mm->getGOTBase();
	}

	void SetDlsymTable(void *ptr) {
	    mm->SetDlsymTable(ptr);
	}

	void *getDlsymTable() const {
	    return mm->getDlsymTable();
	}

	uint8_t *startFunctionBody(const Function *F, 
		uintptr_t &ActualSize) {
	    return mm->startFunctionBody(F, ActualSize);
	}

	uint8_t *allocateStub(const GlobalValue* F, 
		unsigned StubSize, 
		unsigned Alignment) {
	    return mm->allocateStub(F, StubSize, Alignment);
	}

	void endFunctionBody(const Function *F, uint8_t *FunctionStart, 
		uint8_t *FunctionEnd) {
	    mm->endFunctionBody(F, FunctionStart, FunctionEnd);
	    Function *f = const_cast<Function *>(F);
	    RoxorScope *s = RoxorCompiler::shared->scope_for_function(f);
	    functions.push_back(new RoxorFunction(f, s, FunctionStart,
			FunctionEnd));
	}

	void deallocateMemForFunction(const Function *F) {
	    mm->deallocateMemForFunction(F);
	}

	uint8_t* startExceptionTable(const Function* F, 
		uintptr_t &ActualSize) {
	    return mm->startExceptionTable(F, ActualSize);
	}

	void endExceptionTable(const Function *F, uint8_t *TableStart, 
		uint8_t *TableEnd, uint8_t* FrameRegister) {
	    assert(!functions.empty());
	    functions.back()->ehs.push_back(FrameRegister);
	    mm->endExceptionTable(F, TableStart, TableEnd, FrameRegister);
	}

	void setPoisonMemory(bool poison) {
	    mm->setPoisonMemory(poison);
	}
};

extern "C" void *__cxa_allocate_exception(size_t);
extern "C" void __cxa_throw(void *, void *, void (*)(void *));
extern "C" void __cxa_rethrow(void);

RoxorCore::RoxorCore(void)
{
    running = false;
    multithreaded = false;
    abort_on_exception = false;

    pthread_assert(pthread_mutex_init(&gl, 0));

    // Fixnum is an immediate type, no need to retain
    rand_seed = INT2FIX(0);

    load_path = rb_ary_new();
    rb_objc_retain((void *)load_path);

    loaded_features = rb_ary_new();
    rb_objc_retain((void *)loaded_features);

    threads = rb_ary_new();
    rb_objc_retain((void *)threads);

    bs_parser = NULL;

    llvm_start_multithreaded();

    emp = new ExistingModuleProvider(RoxorCompiler::module);
    jmm = new RoxorJITManager;

    InitializeNativeTarget();

    std::string err;
    ee = ExecutionEngine::createJIT(emp, &err, jmm, CodeGenOpt::None, false);
    if (ee == NULL) {
	fprintf(stderr, "error while creating JIT: %s\n", err.c_str());
	abort();
    }
    ee->DisableLazyCompilation();

    fpm = new FunctionPassManager(emp);
    fpm->add(new TargetData(*ee->getTargetData()));

    // Do simple "peephole" optimizations and bit-twiddling optzns.
    fpm->add(createInstructionCombiningPass());
    // Eliminate unnecessary alloca.
    fpm->add(createPromoteMemoryToRegisterPass());
    // Reassociate expressions.
    fpm->add(createReassociatePass());
    // Eliminate Common SubExpressions.
    fpm->add(createGVNPass());
    // Simplify the control flow graph (deleting unreachable blocks, etc).
    fpm->add(createCFGSimplificationPass());
    // Eliminate tail calls.
    fpm->add(createTailCallEliminationPass());

#if ROXOR_VM_DEBUG
    functions_compiled = 0;
#endif
}

RoxorCore::~RoxorCore(void)
{
    // TODO
}

RoxorVM::RoxorVM(void)
{
    current_top_object = Qnil;
    current_class = NULL;
    safe_level = 0;
    backref = Qnil;
    broken_with = Qundef;
    last_status = Qnil;
    errinfo = Qnil;
    parse_in_eval = false;
    has_ensure = false;
    return_from_block = -1;
}

static inline void *
block_cache_key(const rb_vm_block_t *b)
{
    if ((b->flags & VM_BLOCK_IFUNC) == VM_BLOCK_IFUNC) {
	return (void *)b->imp;
    }
    return (void *)b->userdata;
}

RoxorVM::RoxorVM(const RoxorVM &vm)
{
    current_top_object = vm.current_top_object;
    current_class = vm.current_class;
    safe_level = vm.safe_level;

    std::vector<rb_vm_block_t *> &vm_blocks =
	const_cast<RoxorVM &>(vm).current_blocks;

    for (std::vector<rb_vm_block_t *>::iterator i = vm_blocks.begin();
	 (i + 1) != vm_blocks.end();
	 ++i) {

	const rb_vm_block_t *orig = *i;
	rb_vm_block_t *b = NULL;
	if (orig != NULL) {
#if 1
	    b = const_cast<rb_vm_block_t *>(orig);
#else
	    // XXX: This code does not work yet, it raises a failed integrity
	    // check when running the specs.
	    const size_t block_size = sizeof(rb_vm_block_t *)
		+ (orig->dvars_size * sizeof(VALUE *));

	    b = (rb_vm_block_t *)xmalloc(block_size);
	    memcpy(b, orig, block_size);

	    b->proc = orig->proc; // weak
	    GC_WB(&b->self, orig->self);
	    GC_WB(&b->locals, orig->locals);
	    GC_WB(&b->parent_block, orig->parent_block);  // XXX not sure
#endif
	    rb_objc_retain(b);
	    blocks[block_cache_key(orig)] = b;
	}
	current_blocks.push_back(b);
    }

    // TODO bindings, exceptions?

    backref = Qnil;
    broken_with = Qundef;
    last_status = Qnil;
    errinfo = Qnil;
    parse_in_eval = false;
    has_ensure = false;
    return_from_block = -1;
    throw_exc = NULL;
}

RoxorVM::~RoxorVM(void)
{
    for (std::map<void *, rb_vm_block_t *>::iterator i = blocks.begin();
	i != blocks.end();
	++i) {
	GC_RELEASE(i->second);
    }
    blocks.clear();

    GC_RELEASE(backref);
    GC_RELEASE(broken_with);
    GC_RELEASE(last_status);
    GC_RELEASE(errinfo);
}

static void
append_ptr_address(std::string &s, void *ptr)
{
    char buf[100];
    snprintf(buf, sizeof buf, "%p", ptr);
    s.append(buf);
}

std::string
RoxorVM::debug_blocks(void)
{
    std::string s;
    if (current_blocks.empty()) {
	s.append("empty");
    }
    else {
	for (std::vector<rb_vm_block_t *>::iterator i = current_blocks.begin();
	     i != current_blocks.end();
	     ++i) {
	    append_ptr_address(s, *i);
	    s.append(" ");
	}
    }
    return s;
}

std::string
RoxorVM::debug_exceptions(void)
{
    std::string s;
    if (current_exceptions.empty()) {
	s.append("empty");
    }
    else {
	for (std::vector<VALUE>::iterator i = current_exceptions.begin();
	     i != current_exceptions.end();
	     ++i) {
	    append_ptr_address(s, (void *)*i);
	    s.append(" ");
	}
    }
    return s;
}

IMP
RoxorCore::compile(Function *func)
{
    std::map<Function *, IMP>::iterator iter = JITcache.find(func);
    if (iter != JITcache.end()) {
	return iter->second;
    }

#if ROXOR_COMPILER_DEBUG
    // in AOT mode, the verifier is already called
    // (and calling it here would check functions not fully compiled yet)
    if (!ruby_aot_compile) {
	if (verifyModule(*RoxorCompiler::module, PrintMessageAction)) {
	    printf("Error during module verification\n");
	    exit(1);
	}
    }

    uint64_t start = mach_absolute_time();
#endif

    // Optimize & compile.
    optimize(func);
    IMP imp = (IMP)ee->getPointerToFunction(func);
    JITcache[func] = imp;

#if ROXOR_COMPILER_DEBUG
    uint64_t elapsed = mach_absolute_time() - start;

    static mach_timebase_info_data_t sTimebaseInfo;

    if (sTimebaseInfo.denom == 0) {
	(void) mach_timebase_info(&sTimebaseInfo);
    }

    uint64_t elapsedNano = elapsed * sTimebaseInfo.numer / sTimebaseInfo.denom;

    fprintf(stderr, "compilation of LLVM function %p done, took %lld ns\n",
	func, elapsedNano);
#endif

#if ROXOR_VM_DEBUG
    functions_compiled++;
#endif

    return imp;
}

// in libgcc
extern "C" void __deregister_frame(const void *);

void
RoxorCore::delenda(Function *func)
{
    assert(func->use_empty());

#if MAC_OS_X_VERSION_MAX_ALLOWED >= 1060
    // Remove from cache.
    std::map<Function *, IMP>::iterator iter = JITcache.find(func);
    if (iter != JITcache.end()) {
	JITcache.erase(iter);
    }

    // Delete for JIT memory manager list.
    RoxorFunction *f = jmm->delete_function(func);
    assert(f != NULL);

    // Unregister each dwarf exception handler.
    // XXX this should really be done by LLVM...
    for (std::vector<unsigned char *>::iterator i = f->ehs.begin();
	    i != f->ehs.end(); ++i) {
	__deregister_frame((const void *)*i);
    }

    // Remove the compiler scope.
    RoxorCompiler::shared->delete_scope(func);
    delete f;

    // Delete machine code.
    ee->freeMachineCodeForFunction(func);

    // Delete IR.
    func->eraseFromParent();
#endif
}

bool
RoxorCore::symbolize_call_address(void *addr, void **startp, char *path,
	size_t path_len, unsigned long *ln, char *name, size_t name_len)
{
    void *start = NULL;

    RoxorFunction *f = jmm->find_function((unsigned char *)addr);
    if (f != NULL) {
	if (f->imp == NULL) {
	    f->imp = ee->getPointerToFunctionOrStub(f->f);
	}
	start = f->imp;
    }
    else {
	if (!rb_objc_symbolize_address(addr, &start, NULL, 0)) {
	    return false;
	}
    }

    assert(start != NULL);
    if (startp != NULL) {
	*startp = start;
    }

    if (name != NULL || path != NULL || ln != NULL) {
	std::map<IMP, rb_vm_method_node_t *>::iterator iter = 
	    ruby_imps.find((IMP)start);
	if (iter == ruby_imps.end()) {
	    // TODO symbolize objc selectors
	    return false;
	}

	rb_vm_method_node_t *node = iter->second;

	RoxorScope *scope = f == NULL ? NULL : f->scope;
	if (ln != NULL) {
	    if (scope != NULL) {
#if __LP64__
		// So, we need to determine here which call to the dispatcher
		// we are exactly, so that we can retrieve the appropriate
		// line number from the annotation.
		// Unfortunately, the only way to achieve that seems to scan
		// the current function's machine code.
		// This code has only been tested on x86_64 but could be
		// easily ported to i386.
		const uint32_t sym = *(uint32_t *)((unsigned char *)addr - 8);
		const int sentinel = sym & 0xff;

		unsigned char *p = f->start;
		unsigned int i = 0;
		while ((p = (unsigned char *)memchr(p, sentinel,
				(unsigned char *)addr - p)) != NULL) {
		    if (*(uint32_t *)p == sym) {
			i++;
		    }
		    p++;
		}

		if (i > 0 && i - 1 < scope->dispatch_lines.size()) {
		    *ln = scope->dispatch_lines[i - 1];
		}
		else {
		    *ln = 0;
		}
#else
		// TODO 32-bit hack...
		*ln = 0;
#endif
	    }
	    else {
		*ln = 0;
	    }
	}
	if (path != NULL) {
	    if (scope != NULL) {
		strncpy(path, scope->path.c_str(), path_len);
	    }
	    else {
		strncpy(path, "core", path_len);
	    }
	}
	if (name != NULL) {
	    strncpy(name, sel_getName(node->sel), name_len);
	}
    }

    return true;
}

void
RoxorCore::symbolize_backtrace_entry(int index, void **startp, char *path,
	size_t path_len, unsigned long *ln, char *name, size_t name_len)
{
    void *callstack[10];
    const int callstack_n = backtrace(callstack, 10);

    index++; // count us!

    if (callstack_n < index
	|| !GET_CORE()->symbolize_call_address(callstack[index], startp,
		path, path_len, ln, name, name_len)) {
	if (path != NULL) {
	    strncpy(path, "core", path_len);
	}
	if (ln != NULL) {
	    *ln = 0;
	}
    }
}

struct ccache *
RoxorCore::constant_cache_get(ID path)
{
    std::map<ID, struct ccache *>::iterator iter = ccache.find(path);
    if (iter == ccache.end()) {
	struct ccache *cache = (struct ccache *)malloc(sizeof(struct ccache));
	cache->outer = 0;
	cache->val = Qundef;
	ccache[path] = cache;
	return cache;
    }
    return iter->second;
}

extern "C"
void *
rb_vm_get_constant_cache(const char *name)
{
    return GET_CORE()->constant_cache_get(rb_intern(name));
}

struct mcache *
RoxorCore::method_cache_get(SEL sel, bool super)
{
    if (super) {
	struct mcache *cache = (struct mcache *)malloc(sizeof(struct mcache));
	cache->flag = 0;
	// TODO store the cache somewhere and invalidate it appropriately.
	return cache;
    }
    std::map<SEL, struct mcache *>::iterator iter = mcache.find(sel);
    if (iter == mcache.end()) {
	struct mcache *cache = (struct mcache *)malloc(sizeof(struct mcache));
	cache->flag = 0;
	mcache[sel] = cache;
	return cache;
    }
    return iter->second;
}

extern "C"
void *
rb_vm_get_method_cache(SEL sel)
{
    const bool super = strncmp(sel_getName(sel), "__super__:", 10) == 0;
    return GET_CORE()->method_cache_get(sel, super); 
}

rb_vm_method_node_t *
RoxorCore::method_node_get(IMP imp, bool create)
{
    rb_vm_method_node_t *n;
    std::map<IMP, rb_vm_method_node_t *>::iterator iter = ruby_imps.find(imp);
    if (iter == ruby_imps.end()) {
	if (create) {
	    n = (rb_vm_method_node_t *)malloc(sizeof(rb_vm_method_node_t));
	    ruby_imps[imp] = n;
	}
	else {
	    n = NULL;
	}
    }
    else {
	n = iter->second;
    }
    return n;
}

rb_vm_method_node_t *
RoxorCore::method_node_get(Method m, bool create)
{
    rb_vm_method_node_t *n;
    std::map<Method, rb_vm_method_node_t *>::iterator iter =
	ruby_methods.find(m);
    if (iter == ruby_methods.end()) {
	if (create) {
	    n = (rb_vm_method_node_t *)malloc(sizeof(rb_vm_method_node_t));
	    ruby_methods[m] = n;
	}
	else {
	    n = NULL;
	}
    }
    else {
	n = iter->second;
    }
    return n;
}

extern "C"
bool
rb_vm_is_ruby_method(Method m)
{
    return GET_CORE()->method_node_get(m) != NULL;
}

size_t
RoxorCore::get_sizeof(const Type *type)
{
    return ee->getTargetData()->getTypeSizeInBits(type) / 8;
}

size_t
RoxorCore::get_sizeof(const char *type)
{
    return get_sizeof(RoxorCompiler::shared->convert_type(type));
}

bool
RoxorCore::is_large_struct_type(const Type *type)
{
    return type->getTypeID() == Type::StructTyID
	&& ee->getTargetData()->getTypeSizeInBits(type) > 128;
}

inline GlobalVariable *
RoxorCore::redefined_op_gvar(SEL sel, bool create)
{
    std::map <SEL, GlobalVariable *>::iterator iter =
	redefined_ops_gvars.find(sel);
    GlobalVariable *gvar = NULL;
    if (iter == redefined_ops_gvars.end()) {
	if (create) {
	    gvar = new GlobalVariable(*RoxorCompiler::module,
		    Type::getInt1Ty(context),
		    ruby_aot_compile ? true : false,
		    GlobalValue::InternalLinkage,
		    ConstantInt::getFalse(context), "");
	    assert(gvar != NULL);
	    redefined_ops_gvars[sel] = gvar;
	}
    }
    else {
	gvar = iter->second;
    }
    return gvar;
}

inline bool
RoxorCore::should_invalidate_inline_op(SEL sel, Class klass)
{
    if (sel == selEq || sel == selEqq || sel == selNeq) {
	return klass == (Class)rb_cFixnum
	    || klass == (Class)rb_cFloat
	    || klass == (Class)rb_cBignum
	    || klass == (Class)rb_cSymbol
	    || klass == (Class)rb_cNSString
	    || klass == (Class)rb_cNSMutableString
	    || klass == (Class)rb_cNSArray
	    || klass == (Class)rb_cNSMutableArray
	    || klass == (Class)rb_cNSHash
	    || klass == (Class)rb_cNSMutableHash;
    }
    if (sel == selPLUS || sel == selMINUS || sel == selDIV 
	|| sel == selMULT || sel == selLT || sel == selLE 
	|| sel == selGT || sel == selGE) {
	return klass == (Class)rb_cFixnum
	    || klass == (Class)rb_cFloat
	    || klass == (Class)rb_cBignum;
    }
    if (sel == selLTLT || sel == selAREF || sel == selASET) {
	return klass == (Class)rb_cNSArray
	    || klass == (Class)rb_cNSMutableArray;
    }
    if (sel == selSend || sel == sel__send__ || sel == selEval) {
	// Matches any class, since these are Kernel methods.
	return true;
    }

    printf("invalid inline op `%s' to invalidate!\n", sel_getName(sel));
    abort();
}

static ID
sanitize_mid(SEL sel)
{
    const char *selname = sel_getName(sel);
    const size_t sellen = strlen(selname);
    if (selname[sellen - 1] == ':') {
	if (memchr(selname, ':', sellen - 1) != NULL) {
	    return 0;
	}
	char buf[100];
	strncpy(buf, selname, sellen);
	buf[sellen - 1] = '\0';
	return rb_intern(buf);
    }
    return rb_intern(selname);
}

void
RoxorCore::method_added(Class klass, SEL sel)
{
    if (get_running()) {
	// Call method_added: or singleton_method_added:.
	ID mid = sanitize_mid(sel);
	if (mid != 0) {
	    VALUE sym = ID2SYM(mid);
	    if (RCLASS_SINGLETON(klass)) {
		VALUE sk = rb_iv_get((VALUE)klass, "__attached__");
		rb_vm_call(sk, selSingletonMethodAdded, 1, &sym, false);
	    }
	    else {
		rb_vm_call((VALUE)klass, selMethodAdded, 1, &sym, false);
	    }
	}
    }

}

void
RoxorCore::invalidate_method_cache(SEL sel)
{
    std::map<SEL, struct mcache *>::iterator iter = mcache.find(sel);
    if (iter != mcache.end()) {
	iter->second->flag = 0;
    }
}

rb_vm_method_node_t *
RoxorCore::add_method(Class klass, SEL sel, IMP imp, IMP ruby_imp,
	const rb_vm_arity_t &arity, int flags, const char *types)
{
    // #initialize and #initialize_copy are always private.
    if (sel == selInitialize || sel == selInitialize2
	    || sel == selInitializeCopy) {
	flags |= VM_METHOD_PRIVATE;
    }

#if ROXOR_VM_DEBUG
    printf("defining %c[%s %s] with imp %p/%p types %s flags %d arity %d\n",
	    class_isMetaClass(klass) ? '+' : '-',
	    class_getName(klass),
	    sel_getName(sel),
	    imp,
	    ruby_imp,
	    types,
	    flags,
	    arity.real);
#endif

    // Register the implementation into the runtime.
    class_replaceMethod(klass, sel, imp, types);

    // Cache the method.
    Method m = class_getInstanceMethod(klass, sel);
    assert(m != NULL);
    assert(method_getImplementation(m) == imp);
    rb_vm_method_node_t *real_node = method_node_get(m, true);
    real_node->klass = klass;
    real_node->objc_imp = imp;
    real_node->ruby_imp = ruby_imp;
    real_node->arity = arity;
    real_node->flags = flags;
    real_node->sel = sel;

    // Cache the implementation.
    std::map<IMP, rb_vm_method_node_t *>::iterator iter2 = ruby_imps.find(imp);
    rb_vm_method_node_t *node;
    if (iter2 == ruby_imps.end()) {
	node = (rb_vm_method_node_t *)malloc(sizeof(rb_vm_method_node_t));
	node->objc_imp = imp;
	ruby_imps[imp] = node;
    }
    else {
	node = iter2->second;
	assert(node->objc_imp == imp);
    }
    node->klass = klass;
    node->arity = arity;
    node->flags = flags;
    node->sel = sel;
    node->ruby_imp = ruby_imp;
    if (imp != ruby_imp) {
	ruby_imps[ruby_imp] = node;
    }

    // Invalidate respond_to cache.
    invalidate_respond_to_cache();

    // Invalidate dispatch cache.
    invalidate_method_cache(sel);

    // Invalidate inline operations.
    if (running) {
	GlobalVariable *gvar = redefined_op_gvar(sel, false);
	if (gvar != NULL && should_invalidate_inline_op(sel, klass)) {
	    void *val = ee->getOrEmitGlobalVariable(gvar);
#if ROXOR_VM_DEBUG
	    printf("change redefined global for [%s %s] to true\n",
		    class_getName(klass),
		    sel_getName(sel));
#endif
	    assert(val != NULL);
	    *(bool *)val = true;
	}
    }

    // If alloc is redefined, mark the class as such.
    if (sel == selAlloc
	&& (RCLASS_VERSION(klass) & RCLASS_HAS_ROBJECT_ALLOC) 
	== RCLASS_HAS_ROBJECT_ALLOC) {
	RCLASS_SET_VERSION(klass, (RCLASS_VERSION(klass) ^ 
		    RCLASS_HAS_ROBJECT_ALLOC));
    }

    // Forward method definition to the included classes.
    if (RCLASS_VERSION(klass) & RCLASS_IS_INCLUDED) {
	VALUE included_in_classes = rb_attr_get((VALUE)klass, 
		idIncludedInClasses);
	if (included_in_classes != Qnil) {
	    int i, count = RARRAY_LEN(included_in_classes);
	    for (i = 0; i < count; i++) {
		VALUE mod = RARRAY_AT(included_in_classes, i);
#if ROXOR_VM_DEBUG
		printf("forward %c[%s %s] with imp %p node %p types %s\n",
			class_isMetaClass((Class)mod) ? '+' : '-',
			class_getName((Class)mod),
			sel_getName(sel),
			imp,
			node,
			types);
#endif
		class_replaceMethod((Class)mod, sel, imp, types);

		Method m = class_getInstanceMethod((Class)mod, sel);
		assert(m != NULL);
		assert(method_getImplementation(m) == imp);
		node = method_node_get(m, true);
		node->klass = (Class)mod;
		node->objc_imp = imp;
		node->ruby_imp = ruby_imp;
		node->arity = arity;
		node->flags = flags;
		node->sel = sel;
	    }
	}
    }

    return real_node;
}

void
RoxorCore::const_defined(ID path)
{
    // Invalidate constant cache.
    std::map<ID, struct ccache *>::iterator iter = ccache.find(path);
    if (iter != ccache.end()) {
	iter->second->val = Qundef;
    }
}

inline int
RoxorCore::find_ivar_slot(VALUE klass, ID name, bool create)
{
    VALUE k = klass;
    int slot = 0;

    while (k != 0) {
	std::map <ID, int> *slots = get_ivar_slots((Class)k);
	std::map <ID, int>::iterator iter = slots->find(name);
	if (iter != slots->end()) {
#if ROXOR_VM_DEBUG
	    printf("prepare ivar %s slot as %d (already prepared in class %s)\n",
		    rb_id2name(name), iter->second, class_getName((Class)k));
#endif
	    return iter->second;
	}
	slot += slots->size();
	k = RCLASS_SUPER(k);
    }

    if (create) {
#if ROXOR_VM_DEBUG
	printf("prepare ivar %s slot as %d (new in class %s)\n",
		rb_id2name(name), slot, class_getName((Class)klass));
#endif
	get_ivar_slots((Class)klass)->insert(std::pair<ID, int>(name, slot));
	return slot;
    }
    else {
	return -1;
    }
}

void
RoxorCore::each_ivar_slot(VALUE obj, int (*func)(ANYARGS),
	void *ctx)
{
    VALUE k = *(VALUE *)obj;

    while (k != 0) {
	std::map <ID, int> *slots = get_ivar_slots((Class)k, false);
	if (slots != NULL) {
	    for (std::map <ID, int>::iterator iter = slots->begin();
		 iter != slots->end();
		 ++iter) {
		ID name = iter->first;
		int slot = iter->second;
		VALUE value = rb_vm_get_ivar_from_slot(obj, slot);
		if (value != Qundef) {
		    func(name, value, ctx);
		}
	    }
	}
	k = RCLASS_SUPER(k);
    }
}

inline bool
RoxorCore::class_can_have_ivar_slots(VALUE klass)
{
    const long klass_version = RCLASS_VERSION(klass);
    if ((klass_version & RCLASS_IS_RUBY_CLASS) != RCLASS_IS_RUBY_CLASS
	|| (klass_version & RCLASS_IS_OBJECT_SUBCLASS)
	    != RCLASS_IS_OBJECT_SUBCLASS
	|| klass == rb_cClass || klass == rb_cModule) {
	return false;
    }
    return true;
}

extern "C"
bool
rb_vm_running(void)
{
    return GET_CORE()->get_running();
}

extern "C"
void
rb_vm_set_running(bool flag)
{
    GET_CORE()->set_running(flag); 
}

extern "C"
VALUE
rb_vm_rand_seed(void)
{
    VALUE     rand_seed;
    RoxorCore *core = GET_CORE();

    core->lock();
    rand_seed = core->get_rand_seed();
    core->unlock();

    return rand_seed;
}

extern "C"
void
rb_vm_set_rand_seed(VALUE rand_seed)
{
    RoxorCore *core = GET_CORE();

    core->lock();
    if (core->get_rand_seed() != rand_seed) {
	GC_RELEASE(core->get_rand_seed());
	GC_RETAIN(rand_seed);
	core->set_rand_seed(rand_seed);
    }
    core->unlock();
}

extern "C"
bool
rb_vm_abort_on_exception(void)
{
    return GET_CORE()->get_abort_on_exception();
}

extern "C"
void
rb_vm_set_abort_on_exception(bool flag)
{
    GET_CORE()->set_abort_on_exception(flag);
}

extern "C"
void 
rb_vm_set_const(VALUE outer, ID id, VALUE obj, unsigned char dynamic_class)
{
    if (dynamic_class) {
	Class k = GET_VM()->get_current_class();
	if (k != NULL) {
	    outer = (VALUE)k;
	}
    }
#if ROXOR_VM_DEBUG
    printf("define const %s::%s to %p\n", 
	    class_getName((Class)outer), 
	    rb_id2name(id),
	    (void *)obj);
#endif
    rb_const_set(outer, id, obj);
    GET_CORE()->const_defined(id);
}

static inline VALUE
rb_const_get_direct(VALUE klass, ID id)
{
    // Search the given class.
    CFDictionaryRef iv_dict = rb_class_ivar_dict(klass);
    if (iv_dict != NULL) {
retry:
	VALUE value;
	if (CFDictionaryGetValueIfPresent(iv_dict, (const void *)id,
		    (const void **)&value)) {
	    if (value == Qundef && RTEST(rb_autoload_load(klass, id))) {
		goto retry;
	    }
	    return value;
	}
    }
    // Search the included modules.
    VALUE mods = rb_attr_get(klass, idIncludedModules);
    if (mods != Qnil) {
	int i, count = RARRAY_LEN(mods);
	for (i = 0; i < count; i++) {
	    VALUE mod = RARRAY_AT(mods, i);
	    VALUE val = rb_const_get_direct(mod, id);
	    if (val != Qundef) {
		return val;
	    }
	}
    }
    return Qundef;
}

static VALUE
rb_vm_const_lookup(VALUE outer, ID path, bool lexical, bool defined)
{
    if (lexical) {
	// Let's do a lexical lookup before a hierarchical one, by looking for
	// the given constant in all modules under the given outer.
	GET_CORE()->lock();
	struct rb_vm_outer *o = GET_CORE()->get_outer((Class)outer);
	while (o != NULL && o->klass != (Class)rb_cNSObject) {
	    VALUE val = rb_const_get_direct((VALUE)o->klass, path);
	    if (val != Qundef) {
		GET_CORE()->unlock();
		return defined ? Qtrue : val;
	    }
	    o = o->outer;
	}
	GET_CORE()->unlock();
    }

    // Nothing was found earlier so here we do a hierarchical lookup.
    return defined ? rb_const_defined(outer, path) : rb_const_get(outer, path);
}

static inline void
check_if_module(VALUE mod)
{
    switch (TYPE(mod)) {
	case T_CLASS:
	case T_MODULE:
	    break;

	default:
	    rb_raise(rb_eTypeError, "%s is not a class/module",
		    RSTRING_PTR(rb_inspect(mod)));
    }
}

extern "C"
VALUE
rb_vm_get_const(VALUE outer, unsigned char lexical_lookup,
	struct ccache *cache, ID path, unsigned char dynamic_class)
{
    if (dynamic_class) {
	Class k = GET_VM()->get_current_class();
	if (lexical_lookup && k != NULL) {
	    outer = (VALUE)k;
	}
    }

    assert(cache != NULL);

    VALUE val;
    if (cache->outer == outer && cache->val != Qundef) {
	val = cache->val;
    }
    else {
	check_if_module(outer);
	val = rb_vm_const_lookup(outer, path, lexical_lookup, false);
	assert(val != Qundef);
	cache->outer = outer;
	cache->val = val;
    }

    return val;
}

extern "C"
void
rb_vm_const_is_defined(ID path)
{
    GET_CORE()->const_defined(path);
}

struct rb_vm_outer *
RoxorCore::get_outer(Class klass)
{
    std::map<Class, struct rb_vm_outer *>::iterator iter =
	outers.find(klass);
    return iter == outers.end() ? NULL : iter->second;
}

void
RoxorCore::set_outer(Class klass, Class mod) 
{
    struct rb_vm_outer *mod_outer = get_outer(mod);
    struct rb_vm_outer *class_outer = get_outer(klass);
    if (class_outer == NULL || class_outer->outer != mod_outer) {
	if (class_outer != NULL) {
	    free(class_outer);
	}
	class_outer = (struct rb_vm_outer *)
	    malloc(sizeof(struct rb_vm_outer));
	class_outer->klass = klass;
	class_outer->outer = mod_outer;
	outers[klass] = class_outer;
    }
}

extern "C"
void
rb_vm_set_outer(VALUE klass, VALUE under)
{
#if ROXOR_VM_DEBUG
    printf("set outer of %s to %s\n", class_getName((Class)klass),
	    class_getName((Class)under));
#endif
    GET_CORE()->set_outer((Class)klass, (Class)under);
}

extern "C"
VALUE
rb_vm_get_outer(VALUE klass)
{
    rb_vm_outer_t *o = GET_CORE()->get_outer((Class)klass);
    return o == NULL ? Qundef : (VALUE)o->klass;
}

extern "C"
VALUE
rb_vm_define_class(ID path, VALUE outer, VALUE super, int flags,
	unsigned char dynamic_class)
{
    assert(path > 0);
    check_if_module(outer);

    if (dynamic_class) {
	Class k = GET_VM()->get_current_class();
	if (k != NULL) {
	    outer = (VALUE)k;
	}
    }

    VALUE klass;
    if (rb_const_defined_at(outer, path)) {
	klass = rb_const_get_at(outer, path);
	check_if_module(klass);
	if (!(flags & DEFINE_MODULE) && super != 0) {
	    if (rb_class_real(RCLASS_SUPER(klass)) != super) {
		rb_raise(rb_eTypeError, "superclass mismatch for class %s",
			rb_class2name(klass));
	    }
	}
    }
    else {
	if (flags & DEFINE_MODULE) {
	    assert(super == 0);
	    klass = rb_define_module_id(path);
	    rb_set_class_path2(klass, outer, rb_id2name(path),
		    flags & DEFINE_OUTER);
	    rb_const_set(outer, path, klass);
	}
	else {
	    if (super == 0) {
		super = rb_cObject;
	    }
	    else {
		check_if_module(super);
	    }
	    klass = rb_define_class_id(path, super);
	    rb_set_class_path2(klass, outer, rb_id2name(path),
		    flags & DEFINE_OUTER);
	    rb_const_set(outer, path, klass);
	    rb_class_inherited(super, klass);
	}
    }

#if ROXOR_VM_DEBUG
    if (flags & DEFINE_MODULE) {
	printf("define module %s::%s\n", 
		class_getName((Class)outer), 
		rb_id2name(path));
    }
    else {
	printf("define class %s::%s < %s\n", 
		class_getName((Class)outer), 
		rb_id2name(path), 
		class_getName((Class)super));
    }
#endif

    return klass;
}

extern "C"
VALUE
rb_vm_ivar_get(VALUE obj, ID name, int *slot_cache)
{
#if ROXOR_VM_DEBUG
    printf("get ivar <%s %p>.%s slot %d\n",
	    class_getName((Class)CLASS_OF(obj)), (void *)obj,
	    rb_id2name(name), slot_cache == NULL ? -1 : *slot_cache);
#endif
    if (slot_cache == NULL || *slot_cache == -1) {
	return rb_ivar_get(obj, name);
    }
    else {
	VALUE val = rb_vm_get_ivar_from_slot(obj, *slot_cache);
	return val == Qundef ? Qnil : val;
    }
}

extern "C"
void
rb_vm_ivar_set(VALUE obj, ID name, VALUE val, int *slot_cache)
{
#if ROXOR_VM_DEBUG
    printf("set ivar %p.%s slot %d new_val %p\n", (void *)obj, 
	    rb_id2name(name), 
	    slot_cache == NULL ? -1 : *slot_cache,
	    (void *)val);
#endif
    if (slot_cache == NULL || *slot_cache == -1) {
	rb_ivar_set(obj, name, val);
    }
    else {
	rb_vm_set_ivar_from_slot(obj, val, *slot_cache);
    }
}

extern "C"
VALUE
rb_vm_cvar_get(VALUE klass, ID id, unsigned char check,
	unsigned char dynamic_class)
{
    if (dynamic_class) {
	Class k = GET_VM()->get_current_class();
	if (k != NULL) {
	    klass = (VALUE)k;
	}
    }
    return rb_cvar_get2(klass, id, check);
}

extern "C"
VALUE
rb_vm_cvar_set(VALUE klass, ID id, VALUE val, unsigned char dynamic_class)
{
    if (dynamic_class) {
	Class k = GET_VM()->get_current_class();
	if (k != NULL) {
	    klass = (VALUE)k;
	}
    }
    rb_cvar_set(klass, id, val);
    return val;
}

extern "C"
VALUE
rb_vm_ary_cat(VALUE ary, VALUE obj)
{
    if (TYPE(obj) == T_ARRAY) {
	rb_ary_concat(ary, obj);
    }
    else {
	VALUE ary2 = rb_check_convert_type(obj, T_ARRAY, "Array", "to_a");
	if (!NIL_P(ary2)) {
	    rb_ary_concat(ary, ary2);
	}
	else {
	    rb_ary_push(ary, obj);
	}
    }
    return ary;
}

extern "C"
VALUE
rb_vm_to_a(VALUE obj)
{
    VALUE ary = rb_check_convert_type(obj, T_ARRAY, "Array", "to_a");
    if (NIL_P(ary)) {
	ary = rb_ary_new3(1, obj);
    }
    return ary;
}

extern "C"
VALUE
rb_vm_to_ary(VALUE obj)
{
    VALUE ary = rb_check_convert_type(obj, T_ARRAY, "Array", "to_ary");
    if (NIL_P(ary)) {
	ary = rb_ary_new3(1, obj);
    }
    return ary;
}

extern "C" void rb_print_undef(VALUE, ID, int);

static void
rb_vm_alias_method(Class klass, Method method, ID name, bool noargs)
{
    IMP imp = method_getImplementation(method);
    if (UNAVAILABLE_IMP(imp)) {
	return;
    }
    const char *types = method_getTypeEncoding(method);

    rb_vm_method_node_t *node = GET_CORE()->method_node_get(method);
    if (node == NULL) {
	rb_raise(rb_eArgError,
		"only pure Ruby methods can be aliased (`%s' is not)",
		sel_getName(method_getName(method)));
    }

    const char *name_str = rb_id2name(name);
    SEL sel;
    if (noargs) {
	sel = sel_registerName(name_str);
    }
    else {
	char tmp[100];
	snprintf(tmp, sizeof tmp, "%s:", name_str);
	sel = sel_registerName(tmp);
    }

    GET_CORE()->add_method(klass, sel, imp, node->ruby_imp,
	    node->arity, node->flags, types);
}

extern "C"
void
rb_vm_alias2(VALUE outer, ID name, ID def, unsigned char dynamic_class)
{
    if (dynamic_class) {
	Class k = GET_VM()->get_current_class();
	if (k != NULL) {
	    outer = (VALUE)k;
	}
    }
    rb_frozen_class_p(outer);
    if (outer == rb_cObject) {
        rb_secure(4);
    }
    Class klass = (Class)outer;

    const char *def_str = rb_id2name(def);
    SEL sel = sel_registerName(def_str);
    Method def_method1 = class_getInstanceMethod(klass, sel);
    Method def_method2 = NULL;
    if (def_str[strlen(def_str) - 1] != ':') {
	char tmp[100];
	snprintf(tmp, sizeof tmp, "%s:", def_str);
	sel = sel_registerName(tmp);
 	def_method2 = class_getInstanceMethod(klass, sel);
    }

    if (def_method1 == NULL && def_method2 == NULL) {
	rb_print_undef((VALUE)klass, def, 0);
    }
    if (def_method1 != NULL) {
	rb_vm_alias_method(klass, def_method1, name, true);
    }
    if (def_method2 != NULL) {
	rb_vm_alias_method(klass, def_method2, name, false);
    }
}

extern "C"
void
rb_vm_alias(VALUE outer, ID name, ID def)
{
    rb_vm_alias2(outer, name, def, false);
}

extern "C"
void
rb_vm_undef(VALUE klass, ID name, unsigned char dynamic_class)
{
    if (dynamic_class) {
	Class k = GET_VM()->get_current_class();
	if (k != NULL) {
	    klass = (VALUE)k;
	}
    }
    rb_vm_undef_method((Class)klass, name, true);
}

extern "C"
VALUE
rb_vm_defined(VALUE self, int type, VALUE what, VALUE what2)
{
    const char *str = NULL;

    switch (type) {
	case DEFINED_IVAR:
	    if (rb_ivar_defined(self, (ID)what)) {
		str = "instance-variable";
	    }
	    break;

	case DEFINED_GVAR:
	    if (rb_gvar_defined((struct global_entry *)what)) {
		str = "global-variable";
	    }
	    break;

	case DEFINED_CVAR:
	    if (rb_cvar_defined(CLASS_OF(self), (ID)what)) {
		str = "class variable";
	    }
	    break;

	case DEFINED_CONST:
	case DEFINED_LCONST:
	    {
		if (rb_vm_const_lookup(what2, (ID)what,
			    type == DEFINED_LCONST, true)) {
		    str = "constant";
		}
	    }
	    break;

	case DEFINED_SUPER:
	case DEFINED_METHOD:
	    {
		VALUE klass = CLASS_OF(self);
		if (type == DEFINED_SUPER) {
		    klass = RCLASS_SUPER(klass);
		}
		const char *idname = rb_id2name((ID)what);
		SEL sel = sel_registerName(idname);

		bool ok = class_getInstanceMethod((Class)klass, sel) != NULL;
		if (!ok && idname[strlen(idname) - 1] != ':') {
		    char buf[100];
		    snprintf(buf, sizeof buf, "%s:", idname);
		    sel = sel_registerName(buf);
		    ok = class_getInstanceMethod((Class)klass, sel) != NULL;
		}

		if (ok) {
		    str = type == DEFINED_SUPER ? "super" : "method";
		}
	    }
	    break;

	default:
	    printf("unknown defined? type %d", type);
	    abort();
    }

    return str == NULL ? Qnil : rb_str_new2(str);
}

extern "C"
void
rb_vm_prepare_class_ivar_slot(VALUE klass, ID name, int *slot_cache)
{
    assert(slot_cache != NULL);
    assert(*slot_cache == -1);

    if (GET_CORE()->class_can_have_ivar_slots(klass)) {
	*slot_cache = GET_CORE()->find_ivar_slot(klass, name, true);
    }
}

extern "C"
int
rb_vm_find_class_ivar_slot(VALUE klass, ID name)
{
    if (GET_CORE()->class_can_have_ivar_slots(klass)) {
	return GET_CORE()->find_ivar_slot(klass, name, false);
    }
    return -1;
}

extern "C"
void
rb_vm_each_ivar_slot(VALUE obj, int (*func)(ANYARGS), void *ctx)
{
    if (GET_CORE()->class_can_have_ivar_slots(CLASS_OF(obj))) {
	GET_CORE()->each_ivar_slot(obj, func, ctx);	
    } 
}

static inline void 
resolve_method_type(char *buf, const size_t buflen, Class klass, Method m,
	SEL sel, const unsigned int oc_arity)
{
    bs_element_method_t *bs_method = GET_CORE()->find_bs_method(klass, sel);

    if (m == NULL
	|| !rb_objc_get_types(Qnil, klass, sel, m, bs_method, buf, buflen)) {

	std::string *informal_type =
	    GET_CORE()->find_bs_informal_protocol_method(sel,
		    class_isMetaClass(klass));
	if (informal_type != NULL) {
	    strncpy(buf, informal_type->c_str(), buflen);
	}
	else {
	    // Generate an automatic signature, using 'id' (@) for all
	    // arguments. If the method name starts by 'set', we use 'void'
	    // (v) for the return value, otherwise we use 'id' (@).
	    assert(oc_arity < buflen);
	    buf[0] = strncmp(sel_getName(sel), "set", 3) == 0 ? 'v' : '@';
	    buf[1] = '@';
	    buf[2] = ':';
	    for (unsigned int i = 3; i < oc_arity; i++) {
		buf[i] = '@';
	    }
	    buf[oc_arity] = '\0';
	}
    }
    else {
	const unsigned int m_argc = method_getNumberOfArguments(m);
	if (m_argc < oc_arity) {
	    for (unsigned int i = m_argc; i < oc_arity; i++) {
		strcat(buf, "@");
	    }
	}
    }
}

rb_vm_method_node_t *
RoxorCore::retype_method(Class klass, rb_vm_method_node_t *node,
	const char *types)
{
    // TODO: 1) don't reinstall method in case the types didn't change
    // 2) free LLVM machine code from old objc IMP

    // Re-generate ObjC stub. 
    Function *objc_func = RoxorCompiler::shared->compile_objc_stub(NULL,
	    node->ruby_imp, node->arity, types);
    node->objc_imp = compile(objc_func);
    objc_to_ruby_stubs[node->ruby_imp] = node->objc_imp;

    // Re-add the method.
    return add_method(klass, node->sel, node->objc_imp, node->ruby_imp,
	    node->arity, node->flags, types);
}

rb_vm_method_node_t *
RoxorCore::resolve_method(Class klass, SEL sel, Function *func,
	const rb_vm_arity_t &arity, int flags, IMP imp, Method m)
{
    if (imp == NULL) {
	// Compile if necessary.
	assert(func != NULL);
	imp = compile(func);
    }

    // Resolve Objective-C signature.
    const int oc_arity = arity.real + 3;
    char types[100];
    resolve_method_type(types, sizeof types, klass, m, sel, oc_arity);

    // Generate Objective-C stub if needed.
    std::map<IMP, IMP>::iterator iter = objc_to_ruby_stubs.find(imp);
    IMP objc_imp;
    if (iter == objc_to_ruby_stubs.end()) {
	Function *objc_func = RoxorCompiler::shared->compile_objc_stub(func,
		imp, arity, types);
	objc_imp = compile(objc_func);
	objc_to_ruby_stubs[imp] = objc_imp;
    }
    else {
	objc_imp = iter->second;
    }

    // Delete the selector from the not-yet-JIT'ed cache if needed.
    std::multimap<Class, SEL>::iterator iter2, last2;
    iter2 = method_source_sels.find(klass);
    if (iter2 != method_source_sels.end()) {
	last2 = method_source_sels.upper_bound(klass);
	while (iter2 != last2) {
	    if (iter2->second == sel) {
		method_source_sels.erase(iter2);
		break;
	    }
	    ++iter2;
	}
    }

    // Finally, add the method.
    return add_method(klass, sel, objc_imp, imp, arity, flags, types);
}

bool
RoxorCore::resolve_methods(std::map<Class, rb_vm_method_source_t *> *map,
	Class klass, SEL sel)
{
    bool did_something = false;
    std::map<Class, rb_vm_method_source_t *>::iterator iter = map->begin();
    while (iter != map->end()) {
	Class k = iter->first;
	while (k != klass && k != NULL) {
	    k = class_getSuperclass(k);
	}

	if (k != NULL) {
	    rb_vm_method_source_t *m = iter->second;
	    resolve_method(iter->first, sel, m->func, m->arity, m->flags,
		    NULL, NULL);
	    map->erase(iter++);
	    free(m);
	    did_something = true;
	}
	else {
	    ++iter;
	}
    }

    return did_something;
}

extern "C"
bool
rb_vm_resolve_method(Class klass, SEL sel)
{
    if (!GET_CORE()->get_running()) {
	return false;
    }

    GET_CORE()->lock();

    bool status = false;

#if ROXOR_VM_DEBUG
    printf("resolving %c[%s %s]\n",
	class_isMetaClass(klass) ? '+' : '-',
	class_getName(klass),
	sel_getName(sel));
#endif

    std::map<Class, rb_vm_method_source_t *> *map =
	GET_CORE()->method_sources_for_sel(sel, false);
    if (map == NULL) {
	goto bails;
    }

    // Find the class where the method should be defined.
    while (map->find(klass) == map->end() && klass != NULL) {
	klass = class_getSuperclass(klass);
    }
    if (klass == NULL) {
	goto bails;
    }

    // Now let's resolve all methods of the given name on the given class
    // and superclasses.
    status = GET_CORE()->resolve_methods(map, klass, sel);

bails:
    GET_CORE()->unlock();
    return status;
}

void
RoxorCore::prepare_method(Class klass, SEL sel, Function *func,
	const rb_vm_arity_t &arity, int flags)
{
#if ROXOR_VM_DEBUG
    printf("preparing %c[%s %s] on class %p LLVM func %p flags %d\n",
	    class_isMetaClass(klass) ? '+' : '-',
	    class_getName(klass),
	    sel_getName(sel),
	    klass,
	    func,
	    flags);
#endif

    std::map<Class, rb_vm_method_source_t *> *map =
	method_sources_for_sel(sel, true);

    std::map<Class, rb_vm_method_source_t *>::iterator iter = map->find(klass);

    rb_vm_method_source_t *m = NULL;
    if (iter == map->end()) {
	m = (rb_vm_method_source_t *)malloc(sizeof(rb_vm_method_source_t));
	map->insert(std::make_pair(klass, m));
	method_source_sels.insert(std::make_pair(klass, sel));
    }
    else {
	m = iter->second;
    }

    m->func = func;
    m->arity = arity;
    m->flags = flags;

    invalidate_respond_to_cache();
}

static void
prepare_method(Class klass, bool dynamic_class, SEL sel, void *data,
	const rb_vm_arity_t &arity, int flags, bool precompiled)
{
    if (dynamic_class) {
	Class k = GET_VM()->get_current_class();
	if (k != NULL) {
	    const bool meta = class_isMetaClass(klass);
	    klass = k;
	    if (meta && !class_isMetaClass(klass)) {
		klass = *(Class *)klass;
	    }
	}
    }

    const long v = RCLASS_VERSION(klass);
    if (v & RCLASS_SCOPE_PRIVATE) {
	flags |= VM_METHOD_PRIVATE;
    }
    else if (v & RCLASS_SCOPE_PROTECTED) {
	flags |= VM_METHOD_PROTECTED;
    }

    if (sel == sel_ignored) {
	// TODO
	return;
    }

    const char *sel_name = sel_getName(sel);
    const bool genuine_selector = sel_name[strlen(sel_name) - 1] == ':';
    bool redefined = false;
    bool added_modfunc = false;
    SEL orig_sel = sel;
    Method m;
    IMP imp = NULL;

prepare_method:

    m = class_getInstanceMethod(klass, sel);
    if (precompiled) {
	if (imp == NULL) {
	    imp = (IMP)data;
	}
	GET_CORE()->resolve_method(klass, sel, NULL, arity, flags, imp, m);
    }
    else {
	Function *func = (Function *)data;
	if (m != NULL) {
	    // The method already exists - we need to JIT it.
	    if (imp == NULL) {
		imp = GET_CORE()->compile(func);
	    }
	    GET_CORE()->resolve_method(klass, sel, func, arity, flags, imp, m);
	}
	else {
	    // Let's keep the method and JIT it later on demand.
	    GET_CORE()->prepare_method(klass, sel, func, arity, flags);
	}
    }

    if (!redefined) {
	char buf[100];
	SEL new_sel = 0;
	if (!genuine_selector) {
	    snprintf(buf, sizeof buf, "%s:", sel_name);
	    new_sel = sel_registerName(buf);
	    if (arity.max != arity.min) {
		sel = new_sel;
		redefined = true;
		goto prepare_method;
	    }
	}
	else {
	    strlcpy(buf, sel_name, sizeof buf);
	    buf[strlen(buf) - 1] = 0; // remove the ending ':'
	    new_sel = sel_registerName(buf);
	    if (arity.min == 0) {
		sel = new_sel;
		redefined = true;
		goto prepare_method;
	    }
	}
	Method tmp_m = class_getInstanceMethod(klass, new_sel);
	if (tmp_m != NULL) {
	    // If we add -[foo:] and the class responds to -[foo], we need
	    // to disable it (and vice-versa).
	    class_replaceMethod(klass, new_sel,
		    (IMP)rb_vm_undefined_imp, method_getTypeEncoding(tmp_m));	
	}
    }

    if (RCLASS_VERSION(klass) & RCLASS_IS_INCLUDED) {
	VALUE included_in_classes = rb_attr_get((VALUE)klass, 
		idIncludedInClasses);
	if (included_in_classes != Qnil) {
	    int i, count = RARRAY_LEN(included_in_classes);
	    for (i = 0; i < count; i++) {
		VALUE mod = RARRAY_AT(included_in_classes, i);
		rb_vm_set_current_scope(mod, SCOPE_PUBLIC);
		prepare_method((Class)mod, false, orig_sel, data, arity,
			flags, precompiled);
		rb_vm_set_current_scope(mod, SCOPE_DEFAULT);
	    }
	}
    }

    GET_CORE()->method_added(klass, sel);

    if (!added_modfunc && (v & RCLASS_SCOPE_MOD_FUNC)) {
	added_modfunc = true;
	redefined = false;
	klass = *(Class *)klass;
	sel = orig_sel;
	goto prepare_method;
    }
}

extern "C"
void
rb_vm_prepare_method(Class klass, unsigned char dynamic_class, SEL sel,
	Function *func, const rb_vm_arity_t arity, int flags)
{
    prepare_method(klass, dynamic_class, sel, (void *)func, arity,
	    flags, false);
}

extern "C"
void
rb_vm_prepare_method2(Class klass, unsigned char dynamic_class, SEL sel,
	IMP ruby_imp, const rb_vm_arity_t arity, int flags)
{
    prepare_method(klass, dynamic_class, sel, (void *)ruby_imp, arity,
	    flags, true);
}

static rb_vm_method_node_t * __rb_vm_define_method(Class klass, SEL sel,
	IMP objc_imp, IMP ruby_imp, const rb_vm_arity_t &arity, int flags,
	bool direct);

#define VISI(x) ((x)&NOEX_MASK)
#define VISI_CHECK(x,f) (VISI(x) == (f))

static void
push_method(VALUE ary, SEL sel, int flags, int (*filter) (VALUE, ID, VALUE))
{
    if (sel == sel_ignored) {
	return; 
    }

    const char *selname = sel_getName(sel);
    const size_t len = strlen(selname);
    char buf[100];

    const char *p = strchr(selname, ':');
    if (p != NULL && strchr(p + 1, ':') == NULL) {
	// remove trailing ':' for methods with arity 1
	assert(len < sizeof(buf));
	strncpy(buf, selname, len);
	buf[len - 1] = '\0';
	selname = buf;
    }
 
    ID mid = rb_intern(selname);
    VALUE sym = ID2SYM(mid);

    if (rb_ary_includes(ary, sym) == Qfalse) {
	int type = NOEX_PUBLIC;
	if (flags & VM_METHOD_PRIVATE) {
	    type = NOEX_PRIVATE;
	}
	else if (flags & VM_METHOD_PROTECTED) {
	    type = NOEX_PROTECTED;
	}
	(*filter)(sym, type, ary);
    }
} 

rb_vm_method_source_t *
RoxorCore::method_source_get(Class klass, SEL sel)
{
    std::map<SEL, std::map<Class, rb_vm_method_source_t *> *>::iterator iter
	= method_sources.find(sel);
    if (iter != method_sources.end()) {
	std::map<Class, rb_vm_method_source_t *> *m = iter->second;
	std::map<Class, rb_vm_method_source_t *>::iterator iter2
	    = m->find(klass);
	if (iter2 != m->end()) {
	    return iter2->second;
	}
    }
    return NULL;
}

void
RoxorCore::get_methods(VALUE ary, Class klass, bool include_objc_methods,
	int (*filter) (VALUE, ID, VALUE))
{
    // TODO take into account undefined methods

    unsigned int count;
    Method *methods = class_copyMethodList(klass, &count); 
    if (methods != NULL) {
	for (unsigned int i = 0; i < count; i++) {
	    Method m = methods[i];
	    rb_vm_method_node_t *node = method_node_get(m);
	    if (node == NULL && !include_objc_methods) {
		continue;
	    }
	    SEL sel = method_getName(m);
	    push_method(ary, sel, node == NULL ? 0 : node->flags, filter);
	}
	free(methods);
    }

    Class k = klass;
    do {
	std::multimap<Class, SEL>::iterator iter =
	    method_source_sels.find(k);

	if (iter != method_source_sels.end()) {
	    std::multimap<Class, SEL>::iterator last =
		method_source_sels.upper_bound(k);

	    for (; iter != last; ++iter) {
		SEL sel = iter->second;
		rb_vm_method_source_t *src = method_source_get(k, sel);
		assert(src != NULL);
		push_method(ary, sel, src->flags, filter);
	    }
	}

	k = class_getSuperclass(k);
    }
    while (k != NULL);
}

extern "C"
void
rb_vm_push_methods(VALUE ary, VALUE mod, bool include_objc_methods,
		   int (*filter) (VALUE, ID, VALUE))
{
    GET_CORE()->get_methods(ary, (Class)mod, include_objc_methods, filter);
}

extern "C"
void
rb_vm_copy_methods(Class from_class, Class to_class)
{
    GET_CORE()->copy_methods(from_class, to_class);
}

extern "C"
bool
rb_vm_copy_method(Class klass, Method m)
{
    return GET_CORE()->copy_method(klass, m);
}

bool
RoxorCore::copy_method(Class klass, Method m)
{
    rb_vm_method_node_t *node = method_node_get(m);
    if (node == NULL) {
	// Only copy pure-Ruby methods.
	return false;
    }
    SEL sel = method_getName(m);

#if ROXOR_VM_DEBUG
    printf("copy %c[%s %s] from method %p imp %p\n",
	    class_isMetaClass(klass) ? '+' : '-',
	    class_getName(klass),
	    sel_getName(sel),
	    m,
	    method_getImplementation(m));
#endif

    class_replaceMethod(klass, sel, method_getImplementation(m),
	    method_getTypeEncoding(m));

    Method m2 = class_getInstanceMethod(klass, sel);
    assert(m2 != NULL);
    assert(method_getImplementation(m2) == method_getImplementation(m));
    rb_vm_method_node_t *node2 = method_node_get(m2, true);
    memcpy(node2, node, sizeof(rb_vm_method_node_t));

    return true;
}

void
RoxorCore::copy_methods(Class from_class, Class to_class)
{
    Method *methods;
    unsigned int i, methods_count;

    // Copy existing Objective-C methods.
    methods = class_copyMethodList(from_class, &methods_count);
    if (methods != NULL) {
	for (i = 0; i < methods_count; i++) {
	    Method m = methods[i];
	    if (!copy_method(to_class, m)) {
		continue;
	    }

	    SEL sel = method_getName(m);
	    std::map<Class, rb_vm_method_source_t *> *map =
		method_sources_for_sel(sel, false);
	    if (map != NULL) {
		// There might be some non-JIT'ed yet methods on subclasses.
		resolve_methods(map, to_class, sel);
	    }
	}
	free(methods);
    }

    // Copy methods that have not been JIT'ed yet.

    // First, make a list of selectors.
    std::vector<SEL> sels_to_copy;
    std::multimap<Class, SEL>::iterator iter =
	method_source_sels.find(from_class);

    if (iter != method_source_sels.end()) {
	std::multimap<Class, SEL>::iterator last =
	    method_source_sels.upper_bound(from_class);

	for (; iter != last; ++iter) {
	    sels_to_copy.push_back(iter->second);
	}
    }

    // Force a resolving of these selectors on the target class. This must be
    // done outside the next loop since the resolver messes up the Core
    // structures.
    for (std::vector<SEL>::iterator iter = sels_to_copy.begin();
	    iter != sels_to_copy.end();
	    ++iter) {
	class_getInstanceMethod(to_class, *iter);
    }

    // Now, let's really copy the lazy methods.
    std::vector<SEL> sels_to_add;
    for (std::vector<SEL>::iterator iter = sels_to_copy.begin();
	    iter != sels_to_copy.end();
	    ++iter) {
	SEL sel = *iter;

	std::map<Class, rb_vm_method_source_t *> *dict =
	    method_sources_for_sel(sel, false);
	if (dict == NULL) {
	    continue;
	}

	std::map<Class, rb_vm_method_source_t *>::iterator
	    iter2 = dict->find(from_class);
	if (iter2 == dict->end()) {
	    continue;
	}

	rb_vm_method_source_t *m_src = iter2->second;

	Method m = class_getInstanceMethod(to_class, sel);
	if (m != NULL) {
	    // The method already exists on the target class, we need to
	    // JIT it.
	    IMP imp = GET_CORE()->compile(m_src->func);
	    resolve_method(to_class, sel, m_src->func, m_src->arity,
		    m_src->flags, imp, m);
	}
	else {
#if ROXOR_VM_DEBUG
	    printf("lazy copy %c[%s %s] to %c%s\n",
		    class_isMetaClass(from_class) ? '+' : '-',
		    class_getName(from_class),
		    sel_getName(sel),
		    class_isMetaClass(to_class) ? '+' : '-',
		    class_getName(to_class));
#endif

	    rb_vm_method_source_t *m = (rb_vm_method_source_t *)
		malloc(sizeof(rb_vm_method_source_t));
	    m->func = m_src->func;
	    m->arity = m_src->arity;
	    m->flags = m_src->flags;
	    dict->insert(std::make_pair(to_class, m));
	    sels_to_add.push_back(sel);
	}
    }

    for (std::vector<SEL>::iterator i = sels_to_add.begin();
	    i != sels_to_add.end();
	    ++i) {
	method_source_sels.insert(std::make_pair(to_class, *i));
    }
}

extern "C"
bool
rb_vm_lookup_method2(Class klass, ID mid, SEL *psel, IMP *pimp,
		     rb_vm_method_node_t **pnode)
{
    const char *idstr = rb_id2name(mid);
    SEL sel = sel_registerName(idstr);

    if (!rb_vm_lookup_method(klass, sel, pimp, pnode)) {
	char buf[100];
	snprintf(buf, sizeof buf, "%s:", idstr);
	sel = sel_registerName(buf);
	if (!rb_vm_lookup_method(klass, sel, pimp, pnode)) {
	    return false;
	}
    }

    if (psel != NULL) {
	*psel = sel;
    }
    return true;
}

extern "C"
bool
rb_vm_lookup_method(Class klass, SEL sel, IMP *pimp,
		    rb_vm_method_node_t **pnode)
{
    Method m = class_getInstanceMethod(klass, sel);
    if (m == NULL) {
	return false;
    }
    IMP imp = method_getImplementation(m);
    if (UNAVAILABLE_IMP(imp)) {
	return false;
    }
    if (pimp != NULL) {
	*pimp = imp;
    }
    if (pnode != NULL) {
	*pnode = GET_CORE()->method_node_get(m);
    }
    return true;
}

extern "C"
void
rb_vm_define_attr(Class klass, const char *name, bool read, bool write)
{
    assert(klass != NULL);
    assert(read || write);

    char buf[100];
    snprintf(buf, sizeof buf, "@%s", name);
    ID iname = rb_intern(buf);

    if (read) {
	Function *f = RoxorCompiler::shared->compile_read_attr(iname);
	SEL sel = sel_registerName(name);
	rb_vm_prepare_method(klass, false, sel, f, rb_vm_arity(0),
		VM_METHOD_FBODY);
    }

    if (write) {
	Function *f = RoxorCompiler::shared->compile_write_attr(iname);
	snprintf(buf, sizeof buf, "%s=:", name);
	SEL sel = sel_registerName(buf);
	rb_vm_prepare_method(klass, false, sel, f, rb_vm_arity(1),
		VM_METHOD_FBODY);
    }
}

static rb_vm_method_node_t *
__rb_vm_define_method(Class klass, SEL sel, IMP objc_imp, IMP ruby_imp,
	const rb_vm_arity_t &arity, int flags, bool direct)
{
    assert(klass != NULL);

    if (sel == sel_ignored) {
	// TODO
	return NULL;
    }

    const char *sel_name = sel_getName(sel);
    const bool genuine_selector = sel_name[strlen(sel_name) - 1] == ':';
    int oc_arity = genuine_selector ? arity.real : 0;
    bool redefined = direct;
    rb_vm_method_node_t *node;

define_method:
    Method method = class_getInstanceMethod(klass, sel);

    char types[100];
    resolve_method_type(types, sizeof types, klass, method, sel, oc_arity);

    node = GET_CORE()->add_method(klass, sel, objc_imp, ruby_imp, arity,
	    flags, types);

    if (!redefined) {
	if (!genuine_selector && arity.max != arity.min) {
	    char buf[100];
	    snprintf(buf, sizeof buf, "%s:", sel_name);
	    sel = sel_registerName(buf);
	    oc_arity = arity.real;
	    redefined = true;

	    goto define_method;
	}
	else if (genuine_selector && arity.min == 0) {
	    char buf[100];
	    strlcpy(buf, sel_name, sizeof buf);
	    buf[strlen(buf) - 1] = 0; // remove the ending ':'
	    sel = sel_registerName(buf);
	    oc_arity = 0;
	    redefined = true;

	    goto define_method;
	}
    }

    return node;
}

extern "C"
rb_vm_method_node_t * 
rb_vm_define_method(Class klass, SEL sel, IMP imp, NODE *node, bool direct)
{
    assert(node != NULL);

    // TODO: create objc_imp
    return __rb_vm_define_method(klass, sel, imp, imp, rb_vm_node_arity(node),
	    rb_vm_node_flags(node), direct);
}

extern "C"
rb_vm_method_node_t * 
rb_vm_define_method2(Class klass, SEL sel, rb_vm_method_node_t *node,
	long flags, bool direct)
{
    assert(node != NULL);

    
    if (flags == -1) {
	flags = node->flags;
	flags &= ~VM_METHOD_PRIVATE;
	flags &= ~VM_METHOD_PROTECTED;
    }

    return __rb_vm_define_method(klass, sel, node->objc_imp, node->ruby_imp,
	    node->arity, flags, direct);
}

extern "C"
void
rb_vm_define_method3(Class klass, SEL sel, rb_vm_block_t *block)
{
    assert(block != NULL);

    Function *func = RoxorCompiler::shared->compile_block_caller(block);
    IMP imp = GET_CORE()->compile(func);
    NODE *body = rb_vm_cfunc_node_from_imp(klass, -1, imp, 0);
    rb_objc_retain(body);
    rb_objc_retain(block);

    rb_vm_define_method(klass, sel, imp, body, false);
}

void
RoxorCore::undef_method(Class klass, SEL sel)
{
#if ROXOR_VM_DEBUG
    printf("undef %c[%s %s]\n",
	    class_isMetaClass(klass) ? '+' : '-',
	    class_getName(klass),
	    sel_getName(sel));
#endif

    class_replaceMethod((Class)klass, sel, (IMP)rb_vm_undefined_imp, "@@:");
    invalidate_respond_to_cache();

#if 0
    std::map<Method, rb_vm_method_node_t *>::iterator iter
	= ruby_methods.find(m);
    assert(iter != ruby_methods.end());
    free(iter->second);
    ruby_methods.erase(iter);
#endif

    ID mid = sanitize_mid(sel);
    if (mid != 0) {
	VALUE sym = ID2SYM(mid);
	if (RCLASS_SINGLETON(klass)) {
	    VALUE sk = rb_iv_get((VALUE)klass, "__attached__");
	    rb_vm_call(sk, selSingletonMethodUndefined, 1, &sym, false);
	}
	else {
	    rb_vm_call((VALUE)klass, selMethodUndefined, 1, &sym, false);
	}
    }
}

extern "C"
void
rb_vm_undef_method(Class klass, ID name, bool must_exist)
{
    rb_vm_method_node_t *node = NULL;

    if (!rb_vm_lookup_method2((Class)klass, name, NULL, NULL, &node)) {
	if (must_exist) {
	    rb_raise(rb_eNameError, "undefined method `%s' for %s `%s'",
		    rb_id2name(name),
		    TYPE(klass) == T_MODULE ? "module" : "class",
		    rb_class2name((VALUE)klass));
	}
	const char *namestr = rb_id2name(name);
	SEL sel = sel_registerName(namestr);
	GET_CORE()->undef_method(klass, sel);
    }
    else if (node == NULL) {
	if (must_exist) {
	    rb_raise(rb_eRuntimeError,
		    "cannot undefine method `%s' because it is a native method",
		    rb_id2name(name));
	}
    }
    else {
	GET_CORE()->undef_method(klass, node->sel);
    }
}

void
RoxorCore::remove_method(Class klass, SEL sel)
{
#if ROXOR_VM_DEBUG
    printf("remove %c[%s %s]\n",
	    class_isMetaClass(klass) ? '+' : '-',
	    class_getName(klass),
	    sel_getName(sel));
#endif

    Method m = class_getInstanceMethod(klass, sel);
    assert(m != NULL);
    method_setImplementation(m, (IMP)rb_vm_removed_imp);
    invalidate_respond_to_cache();

    ID mid = sanitize_mid(sel);
    if (mid != 0) {
	VALUE sym = ID2SYM(mid);
	if (RCLASS_SINGLETON(klass)) {
	    VALUE sk = rb_iv_get((VALUE)klass, "__attached__");
	    rb_vm_call(sk, selSingletonMethodRemoved, 1, &sym, false);
	}
	else {
	    rb_vm_call((VALUE)klass, selMethodRemoved, 1, &sym, false);
	}
    }
}

extern "C"
void
rb_vm_remove_method(Class klass, ID name)
{
    rb_vm_method_node_t *node = NULL;

    if (!rb_vm_lookup_method2((Class)klass, name, NULL, NULL, &node)) {
	rb_raise(rb_eNameError, "undefined method `%s' for %s `%s'",
		rb_id2name(name),
		TYPE(klass) == T_MODULE ? "module" : "class",
		rb_class2name((VALUE)klass));
    }
    if (node == NULL) {
	rb_raise(rb_eRuntimeError,
		"cannot remove method `%s' because it is a native method",
		rb_id2name(name));
    }
    if (node->klass != klass) {
	rb_raise(rb_eNameError, "method `%s' not defined in %s",
		rb_id2name(name), rb_class2name((VALUE)klass));
    }

    GET_CORE()->remove_method(klass, node->sel);
}

extern "C"
VALUE
rb_vm_masgn_get_elem_before_splat(VALUE ary, int offset)
{
    if (offset < RARRAY_LEN(ary)) {
	return RARRAY_AT(ary, offset);
    }
    return Qnil;
}

extern "C"
VALUE
rb_vm_masgn_get_elem_after_splat(VALUE ary, int before_splat_count, int after_splat_count, int offset)
{
    int len = RARRAY_LEN(ary);
    if (len < before_splat_count + after_splat_count) {
	offset += before_splat_count;
	if (offset < len) {
	    return RARRAY_AT(ary, offset);
	}
    }
    else {
	offset += len - after_splat_count;
	return RARRAY_AT(ary, offset);
    }
    return Qnil;
}

extern "C"
VALUE
rb_vm_masgn_get_splat(VALUE ary, int before_splat_count, int after_splat_count) {
    int len = RARRAY_LEN(ary);
    if (len > before_splat_count + after_splat_count) {
	return rb_ary_subseq(ary, before_splat_count, len - before_splat_count - after_splat_count);
    }
    else {
	return rb_ary_new();
    }
}

extern "C"
VALUE
rb_vm_method_missing(VALUE obj, int argc, const VALUE *argv)
{
    if (argc == 0 || !SYMBOL_P(argv[0])) {
        rb_raise(rb_eArgError, "no id given");
    }

    const rb_vm_method_missing_reason_t last_call_status =
	GET_VM()->get_method_missing_reason();
    const char *format = NULL;
    VALUE exc = rb_eNoMethodError;

    switch (last_call_status) {
	case METHOD_MISSING_PRIVATE:
	    format = "private method `%s' called for %s";
	    break;

	case METHOD_MISSING_PROTECTED:
	    format = "protected method `%s' called for %s";
	    break;

	case METHOD_MISSING_VCALL:
	    format = "undefined local variable or method `%s' for %s";
	    exc = rb_eNameError;
	    break;

	case METHOD_MISSING_SUPER:
	    format = "super: no superclass method `%s' for %s";
	    break;

	case METHOD_MISSING_DEFAULT:
	default:
	    format = "undefined method `%s' for %s";
	    break;
    }

    VALUE meth = rb_sym_to_s(argv[0]);
    if (!rb_vm_respond_to(obj, selToS, true)) {
	// In case #to_s was undefined on the object, let's generate a
	// basic string based on it, because otherwise the following code
	// will raise a #method_missing which will result in an infinite loop.
	obj = rb_any_to_s(obj);
    }

    int n = 0;
    VALUE args[3];
    args[n++] = rb_funcall(rb_cNameErrorMesg, '!', 3, rb_str_new2(format),
	    obj, meth);
    args[n++] = meth;
    if (exc == rb_eNoMethodError) {
	args[n++] = rb_ary_new4(argc - 1, argv + 1);
    }

    exc = rb_class_new_instance(n, args, exc);
    rb_exc_raise(exc);

    abort(); // never reached
}

void *
RoxorCore::gen_stub(std::string types, bool variadic, int min_argc,
	bool is_objc)
{
    lock();

#if ROXOR_VM_DEBUG
    printf("gen Ruby -> %s stub with types %s\n", is_objc ? "ObjC" : "C",
	    types.c_str());
#endif

    std::map<std::string, void *> &stubs = is_objc ? objc_stubs : c_stubs;
    std::map<std::string, void *>::iterator iter = stubs.find(types);
    void *stub;
    if (iter == stubs.end()) {
	Function *f = RoxorCompiler::shared->compile_stub(types.c_str(),
		variadic, min_argc, is_objc);
	stub = (void *)compile(f);
	stubs.insert(std::make_pair(types, stub));
    }
    else {
	stub = iter->second;
    }

    unlock();

    return stub;
}

void *
RoxorCore::gen_to_rval_convertor(std::string type)
{
    std::map<std::string, void *>::iterator iter =
	to_rval_convertors.find(type);
    if (iter != to_rval_convertors.end()) {
	return iter->second;
    }

    Function *f = RoxorCompiler::shared->compile_to_rval_convertor(
	    type.c_str());
    void *convertor = (void *)compile(f);
    to_rval_convertors.insert(std::make_pair(type, convertor));
    
    return convertor; 
}

void *
RoxorCore::gen_to_ocval_convertor(std::string type)
{
    std::map<std::string, void *>::iterator iter =
	to_ocval_convertors.find(type);
    if (iter != to_ocval_convertors.end()) {
	return iter->second;
    }

    Function *f = RoxorCompiler::shared->compile_to_ocval_convertor(
	    type.c_str());
    void *convertor = (void *)compile(f);
    to_ocval_convertors.insert(std::make_pair(type, convertor));
    
    return convertor; 
}

extern "C"
void *
rb_vm_get_block(VALUE obj)
{
    if (obj == Qnil) {
	return NULL;
    }

    VALUE proc = rb_check_convert_type(obj, T_DATA, "Proc", "to_proc");
    if (NIL_P(proc)) {
	rb_raise(rb_eTypeError,
		"wrong argument type %s (expected Proc)",
		rb_obj_classname(obj));
    }
    return rb_proc_get_block(proc);
}

extern "C"
void*
rb_gc_read_weak_ref(void **referrer);

extern "C"
void
rb_gc_assign_weak_ref(const void *value, void *const*location);

static const int VM_LVAR_USES_SIZE = 8;
enum {
    VM_LVAR_USE_TYPE_BLOCK   = 1,
    VM_LVAR_USE_TYPE_BINDING = 2
};
struct rb_vm_var_uses {
    int uses_count;
    void *uses[VM_LVAR_USES_SIZE];
    unsigned char use_types[VM_LVAR_USES_SIZE];
    struct rb_vm_var_uses *next;
};

static void
rb_vm_add_lvar_use(rb_vm_var_uses **var_uses, void *use, unsigned char use_type)
{
    if (var_uses == NULL) {
	return;
    }

    if ((*var_uses == NULL)
	|| ((*var_uses)->uses_count == VM_LVAR_USES_SIZE)) {

	rb_vm_var_uses *new_uses =
	    (rb_vm_var_uses *)malloc(sizeof(rb_vm_var_uses));
	new_uses->next = *var_uses;
	new_uses->uses_count = 0;
	*var_uses = new_uses;
    }
    int current_index = (*var_uses)->uses_count;
    rb_gc_assign_weak_ref(use, &(*var_uses)->uses[current_index]);
    (*var_uses)->use_types[current_index] = use_type;
    ++(*var_uses)->uses_count;
}

extern "C"
void
rb_vm_add_block_lvar_use(rb_vm_block_t *block)
{
    for (rb_vm_block_t *block_for_uses = block;
	 block_for_uses != NULL;
	 block_for_uses = block_for_uses->parent_block) {

	rb_vm_add_lvar_use(block_for_uses->parent_var_uses, block,
		VM_LVAR_USE_TYPE_BLOCK);
    }
}

static void
rb_vm_add_binding_lvar_use(rb_vm_binding_t *binding, rb_vm_block_t *block,
	rb_vm_var_uses **parent_var_uses)
{
    for (rb_vm_block_t *block_for_uses = block;
	 block_for_uses != NULL;
	 block_for_uses = block_for_uses->parent_block) {

	rb_vm_add_lvar_use(block_for_uses->parent_var_uses, binding,
		VM_LVAR_USE_TYPE_BINDING);
    }
    rb_vm_add_lvar_use(parent_var_uses, binding, VM_LVAR_USE_TYPE_BINDING);
}

struct rb_vm_kept_local {
    ID name;
    VALUE *stack_address;
    VALUE *new_address;
};

extern "C"
void
rb_vm_keep_vars(rb_vm_var_uses *uses, int lvars_size, ...)
{
    rb_vm_var_uses *current = uses;
    int use_index;

    while (current != NULL) {
	for (use_index = 0; use_index < current->uses_count; ++use_index) {
	    if (rb_gc_read_weak_ref(&current->uses[use_index]) != NULL) {
		goto use_found;
	    }
	}

	void *old_current = current;
	current = current->next;
	free(old_current);
    }
    // there's no use alive anymore so nothing to do
    return;

use_found:
    rb_vm_kept_local *locals = (rb_vm_kept_local *)alloca(sizeof(rb_vm_kept_local)*lvars_size);

    va_list ar;
    va_start(ar, lvars_size);
    for (int i = 0; i < lvars_size; ++i) {
	locals[i].name = va_arg(ar, ID);
	locals[i].stack_address = va_arg(ar, VALUE *);
	locals[i].new_address = (VALUE *)xmalloc(sizeof(VALUE));
	GC_WB(locals[i].new_address, *locals[i].stack_address);
    }
    va_end(ar);

    while (current != NULL) {
	for (; use_index < current->uses_count; ++use_index) {
	    void *use = rb_gc_read_weak_ref(&current->uses[use_index]);
	    if (use != NULL) {
		unsigned char type = current->use_types[use_index];
		rb_vm_local_t *locals_to_replace;
		if (type == VM_LVAR_USE_TYPE_BLOCK) {
		    rb_vm_block_t *block = (rb_vm_block_t *)use;
		    for (int dvar_index = 0; dvar_index < block->dvars_size; ++dvar_index) {
			for (int lvar_index = 0; lvar_index < lvars_size; ++lvar_index) {
			    if (block->dvars[dvar_index] == locals[lvar_index].stack_address) {
				GC_WB(&block->dvars[dvar_index], locals[lvar_index].new_address);
				break;
			    }
			}
		    }

		    // the parent pointers can't be used anymore
		    block->parent_block = NULL;
		    block->parent_var_uses = NULL;

		    locals_to_replace = block->locals;
		}
		else { // VM_LVAR_USE_TYPE_BINDING
		    rb_vm_binding_t *binding = (rb_vm_binding_t *)use;
		    locals_to_replace = binding->locals;
		}

		for (rb_vm_local_t *l = locals_to_replace; l != NULL; l = l->next) {
		    for (int lvar_index = 0; lvar_index < lvars_size; ++lvar_index) {
			if (l->value == locals[lvar_index].stack_address) {
			    GC_WB(&l->value, locals[lvar_index].new_address);
			    break;
			}
		    }
		}

		// indicate to the GC that we do not have a reference here anymore
		rb_gc_assign_weak_ref(NULL, &current->uses[use_index]);
	    }
	}
	void *old_current = current;
	current = current->next;
	use_index = 0;
	free(old_current);
    }
}

static inline rb_vm_local_t **
push_local(rb_vm_local_t **l, ID name, VALUE *value)
{
    GC_WB(l, xmalloc(sizeof(rb_vm_local_t)));
    (*l)->name = name;
    (*l)->value = value;
    (*l)->next = NULL;
    return &(*l)->next;
}

extern "C"
void
rb_vm_push_binding(VALUE self, rb_vm_block_t *current_block,
		   rb_vm_var_uses **parent_var_uses,
		   int lvars_size, ...)
{
    rb_vm_binding_t *binding =
	(rb_vm_binding_t *)xmalloc(sizeof(rb_vm_binding_t));
    GC_WB(&binding->self, self);

    rb_vm_local_t **l = &binding->locals;

    for (rb_vm_block_t *b = current_block; b != NULL; b = b->parent_block) {
	for (rb_vm_local_t *li = b->locals; li != NULL; li = li->next) {
	    l = push_local(l, li->name, li->value);
	}
    }

    va_list ar;
    va_start(ar, lvars_size);
    for (int i = 0; i < lvars_size; ++i) {
	ID name = va_arg(ar, ID);
	VALUE *value = va_arg(ar, VALUE *);
	l = push_local(l, name, value);
    }
    va_end(ar);

    rb_vm_add_binding_lvar_use(binding, current_block, parent_var_uses);

    RoxorVM *vm = GET_VM();
    GC_WB(&binding->block, vm->current_block());

    vm->push_current_binding(binding);
}

extern "C"
rb_vm_binding_t *
rb_vm_current_binding(void)
{
    return GET_VM()->current_binding();
}

extern "C"
void
rb_vm_add_binding(rb_vm_binding_t *binding)
{
    GET_VM()->push_current_binding(binding, false);
}

extern "C"
void
rb_vm_pop_binding(void)
{
    GET_VM()->pop_current_binding(false);
}

extern "C"
void *
rb_vm_get_call_cache(SEL sel)
{
    return GET_CORE()->method_cache_get(sel, false);
}

extern "C"
void *
rb_vm_get_call_cache2(SEL sel, unsigned char super)
{
    return GET_CORE()->method_cache_get(sel, super);
}

// Should be used inside a method implementation.
extern "C"
int
rb_block_given_p(void)
{
    return GET_VM()->current_block() != NULL ? Qtrue : Qfalse;
}

// Should only be used by Proc.new.
extern "C"
rb_vm_block_t *
rb_vm_first_block(void)
{
    return GET_VM()->first_block();
}

// Should only be used by #block_given?
extern "C"
bool
rb_vm_block_saved(void)
{
    return GET_VM()->previous_block() != NULL;
}

extern "C"
rb_vm_block_t *
rb_vm_current_block(void)
{
    return GET_VM()->current_block();
}

extern "C"
VALUE
rb_vm_current_block_object(void)
{
    rb_vm_block_t *b = GET_VM()->current_block();
    if (b != NULL) {
	return rb_proc_alloc_with_block(rb_cProc, b);
    }
    return Qnil;
}

extern "C"
rb_vm_block_t *
rb_vm_create_block_from_method(rb_vm_method_t *method)
{
    rb_vm_block_t *b = (rb_vm_block_t *)xmalloc(sizeof(rb_vm_block_t));

    b->proc = Qnil;
    GC_WB(&b->self, method->recv);
    b->klass = 0;
    b->arity = method->node == NULL
	? rb_vm_arity(method->arity) : method->node->arity;
    b->imp = (IMP)method;
    b->flags = VM_BLOCK_PROC | VM_BLOCK_METHOD;
    b->locals = NULL;
    b->parent_var_uses = NULL;
    b->parent_block = NULL;
    b->dvars_size = 0;

    return b;
}

static VALUE
rb_vm_block_call_sel(VALUE rcv, SEL sel, VALUE **dvars, rb_vm_block_t *b,
	VALUE x)
{
    if (x == Qnil) {
	rb_raise(rb_eArgError, "no receiver given");
    }
    return rb_vm_call(x, (SEL)dvars[0], 0, NULL, false);
}

extern "C"
rb_vm_block_t *
rb_vm_create_block_calling_sel(SEL sel)
{
    rb_vm_block_t *b = (rb_vm_block_t *)xmalloc(sizeof(rb_vm_block_t)
	    + sizeof(VALUE *));

    b->klass = 0;
    b->proc = Qnil;
    b->arity = rb_vm_arity(1);
    b->flags = VM_BLOCK_PROC;
    b->imp = (IMP)rb_vm_block_call_sel;
    b->dvars[0] = (VALUE *)sel;

    return b;
}

static VALUE
rb_vm_block_curry(VALUE rcv, SEL sel, VALUE **dvars, rb_vm_block_t *b,
	VALUE args)
{
    VALUE proc = (VALUE)dvars[0];
    VALUE passed = (VALUE)dvars[1];
    VALUE arity = (VALUE)dvars[2];

    passed = rb_ary_plus(passed, args);
    rb_ary_freeze(passed);
    if (RARRAY_LEN(passed) < FIX2INT(arity)) {
	return rb_vm_make_curry_proc(proc, passed, arity);
    }
    return rb_proc_call(proc, passed);
}

extern "C"
VALUE
rb_vm_make_curry_proc(VALUE proc, VALUE passed, VALUE arity)
{
    // Proc.new { |*args| curry... }
    rb_vm_block_t *b = (rb_vm_block_t *)xmalloc(sizeof(rb_vm_block_t)
	    + (3 * sizeof(VALUE *)));

    b->klass = 0;
    b->proc = Qnil;
    b->arity.min = 0;
    b->arity.max = -1;
    b->arity.left_req = 0;
    b->arity.real = 1;
    b->flags = VM_BLOCK_PROC;
    b->imp = (IMP)rb_vm_block_curry;
    b->dvars[0] = (VALUE *)proc;
    b->dvars[1] = (VALUE *)passed;
    b->dvars[2] = (VALUE *)arity;

    return rb_proc_alloc_with_block(rb_cProc, b);
}

#if MAC_OS_X_VERSION_MAX_ALLOWED < 1060
// the function is available on Leopard but it's not declared
extern "C" id _objc_msgForward(id receiver, SEL sel, ...);
#endif

#if 0
static inline IMP
class_respond_to(Class klass, SEL sel)
{
    IMP imp = class_getMethodImplementation(klass, sel);
    if (imp == _objc_msgForward) {
	if (rb_vm_resolve_method(klass, sel)) {
	    imp = class_getMethodImplementation(klass, sel);
	}
	else {
	    imp = NULL;
	}
    }
    return imp;
}
#endif

extern "C" VALUE rb_reg_match_pre(VALUE match, SEL sel);
extern "C" VALUE rb_reg_match_post(VALUE match, SEL sel);

extern "C"
VALUE
rb_vm_get_special(char code)
{
    VALUE backref = rb_backref_get();
    if (backref == Qnil) {
	return Qnil;
    }

    VALUE val;
    switch (code) {
	case '&':
	    val = rb_reg_last_match(backref);
	    break;
	case '`':
	    val = rb_reg_match_pre(backref, 0);
	    break;
	case '\'':
	    val = rb_reg_match_post(backref, 0);
	    break;
	case '+':
	    val = rb_reg_match_last(backref);
	    break;
	default:
	    {
		int index = (int)code;
		assert(index > 0 && index < 10);
		val = rb_reg_nth_match(index, backref);
	    }
	    break;
    }

    return val;
}

extern "C"
VALUE
rb_iseq_compile(VALUE src, VALUE file, VALUE line)
{
    // TODO
    return NULL;
}

extern "C"
VALUE
rb_iseq_eval(VALUE iseq)
{
    // TODO
    return Qnil;
}

extern "C"
VALUE
rb_iseq_new(NODE *node, VALUE filename)
{
    // TODO
    return Qnil;
}

static inline void
__vm_raise(void)
{
    VALUE rb_exc = GET_VM()->current_exception();
    // DTrace probe: raise
    if (MACRUBY_RAISE_ENABLED()) {
	char *classname = (char *)rb_class2name(CLASS_OF(rb_exc));
	char file[PATH_MAX];
	unsigned long line = 0;
	GET_CORE()->symbolize_backtrace_entry(2, NULL, file, sizeof file,
		&line, NULL, 0);
	MACRUBY_RAISE(classname, file, line);
    } 
#if __LP64__
    // In 64-bit, an Objective-C exception is a C++ exception.
    id exc = rb_rb2oc_exception(rb_exc);
    objc_exception_throw(exc);
#else
    void *exc = __cxa_allocate_exception(0);
    __cxa_throw(exc, NULL, NULL);
#endif
}

#if !__LP64__
extern "C"
void
rb2oc_exc_handler(void)
{
    VALUE exc = GET_VM()->current_exception();
    if (exc != Qnil) {
	id ocexc = rb_rb2oc_exception(exc);
	objc_exception_throw(ocexc);
    }
    else {
	__cxa_rethrow();
    }
}
#endif

extern "C"
void
rb_vm_raise_current_exception(void)
{
    VALUE exception = GET_VM()->current_exception();
    assert(exception != Qnil);
    rb_iv_set(exception, "bt", rb_vm_backtrace(100));
    __vm_raise(); 
}

extern "C"
void
rb_vm_raise(VALUE exception)
{
    rb_iv_set(exception, "bt", rb_vm_backtrace(100));
    rb_objc_retain((void *)exception);
    GET_VM()->push_current_exception(exception);
    __vm_raise();
}

extern "C"
VALUE
rb_rescue2(VALUE (*b_proc) (ANYARGS), VALUE data1,
           VALUE (*r_proc) (ANYARGS), VALUE data2, ...)
{
    try {
	return (*b_proc)(data1);
    }
    catch (...) {
	VALUE exc = rb_vm_current_exception();
	if (exc != Qnil) {
	    va_list ar;
	    VALUE eclass;
	    bool handled = false;

	    va_start(ar, data2);
	    while ((eclass = va_arg(ar, VALUE)) != 0) {
		if (rb_obj_is_kind_of(exc, eclass)) {
		    handled = true;
		    break;
		}
	    }
	    va_end(ar);

	    if (handled) {
		if (r_proc != NULL) {
		    return (*r_proc)(data2);
		}
		return Qnil;
	    }
	}
	throw;
    }
    return Qnil; // never reached
}

extern "C"
VALUE
rb_ensure(VALUE (*b_proc)(ANYARGS), VALUE data1,
	VALUE (*e_proc)(ANYARGS), VALUE data2)
{
    struct Finally {
	VALUE (*e_proc)(ANYARGS);
	VALUE data2;
	Finally(VALUE (*_e_proc)(ANYARGS), VALUE _data2) {
	    e_proc = _e_proc;
	    data2 = _data2;
	}
	~Finally() { (*e_proc)(data2); }
    } finalizer(e_proc, data2);

    return (*b_proc)(data1);
}

extern "C"
void
rb_vm_break(VALUE val)
{
#if 0
    // XXX this doesn't work yet since break is called inside the block and
    // we do not have a reference to it. This isn't very important though,
    // but since 1.9 doesn't support break without Proc objects we should also
    // raise a similar exception.
    assert(GET_VM()->current_block != NULL);
    if (GET_VM()->current_block->flags & VM_BLOCK_PROC) {
	rb_raise(rb_eLocalJumpError, "break from proc-closure");
    }
#endif
    RoxorVM *vm = GET_VM();
    if (vm->get_broken_with() != val) {
	GC_RELEASE(vm->get_broken_with());
	vm->set_broken_with(val);
	GC_RETAIN(val);
    }
}

extern "C"
VALUE
rb_vm_get_broken_value(void)
{
    return GET_VM()->get_broken_with();
}

extern "C"
VALUE
rb_vm_pop_broken_value(void)
{
    return GET_VM()->pop_broken_with();
}

extern "C"
unsigned char
rb_vm_set_has_ensure(unsigned char state)
{
    RoxorVM *vm = GET_VM();
    const bool old_state = vm->get_has_ensure();
    vm->set_has_ensure(state);
    return old_state ? 1 : 0;
}

extern "C"
void
rb_vm_return_from_block(VALUE val, int id, rb_vm_block_t *running_block)
{
    RoxorVM *vm = GET_VM();

    // Do not trigger a return from the calling scope if the running block
    // is a lambda, to conform to the ruby 1.9 specifications.
    if (running_block->flags & VM_BLOCK_LAMBDA) {
	return;
    }

    // If we are inside an ensure block or if the running block is a Proc,
    // let's implement return-from-block using a C++ exception (slow).
    if (vm->get_has_ensure() || (running_block->flags & VM_BLOCK_PROC)) {
	RoxorReturnFromBlockException *exc =
	    new RoxorReturnFromBlockException();
	exc->val = val;
	exc->id = id;
	throw exc;
    }

    // Otherwise, let's mark the VM (fast).
    vm->set_return_from_block(id);
    if (vm->get_broken_with() != val) {
	GC_RELEASE(vm->get_broken_with());
	vm->set_broken_with(val);
	GC_RETAIN(val);
    }
}

extern "C"
VALUE
rb_vm_returned_from_block(int id)
{
    RoxorVM *vm = GET_VM();
    if (id != -1 && vm->get_return_from_block() == id) {
	vm->set_return_from_block(-1);
    }
    return vm->pop_broken_with();
}

extern "C" std::type_info *__cxa_current_exception_type(void);

static inline bool
current_exception_is_return_from_block(void)
{
    const std::type_info *exc_type = __cxa_current_exception_type();
    return exc_type != NULL
	&& *exc_type == typeid(RoxorReturnFromBlockException *);
}

extern "C"
VALUE
rb_vm_check_return_from_block_exc(RoxorReturnFromBlockException **pexc, int id)
{
    if (current_exception_is_return_from_block()) {
	RoxorReturnFromBlockException *exc = *pexc;
	if (id == -1 || exc->id == id) {
	    VALUE val = exc->val;
	    delete exc;
	    return val;
	}
    }
    return Qundef;
}

static inline void
rb_vm_rethrow(void)
{
    void *exc = __cxa_allocate_exception(0);
    __cxa_throw(exc, NULL, NULL);
}

extern "C"
VALUE
rb_vm_backtrace(int level)
{
    void *callstack[128];
    int callstack_n = backtrace(callstack, 128);

    // TODO should honor level

    VALUE ary = rb_ary_new();

    for (int i = 0; i < callstack_n; i++) {
	char path[PATH_MAX];
	char name[100];
	unsigned long ln = 0;

	if (GET_CORE()->symbolize_call_address(callstack[i], NULL,
		    path, sizeof path, &ln, name, sizeof name)) {
	    char entry[PATH_MAX];
	    if (ln == 0) {
		snprintf(entry, sizeof entry, "%s:in `%s'",
			path, name);
	    }
	    else {
		snprintf(entry, sizeof entry, "%s:%ld:in `%s'",
			path, ln, name);
	    }
	    rb_ary_push(ary, rb_str_new2(entry));
	}
    }

    return ary;
}

extern "C"
unsigned char
rb_vm_is_eh_active(int argc, ...)
{
    assert(argc > 0);

    VALUE current_exception = GET_VM()->current_exception();
    if (current_exception == Qnil) {
	// Not a Ruby exception...
	return 0;
    }

    va_list ar;
    unsigned char active = 0;

    va_start(ar, argc);
    for (int i = 0; i < argc && active == 0; ++i) {
	VALUE obj = va_arg(ar, VALUE);
	if (TYPE(obj) == T_ARRAY) {
	    for (int j = 0, count = RARRAY_LEN(obj); j < count; ++j) {
		VALUE obj2 = RARRAY_AT(obj, j);
		if (rb_obj_is_kind_of(current_exception, obj2)) {
		    active = 1;
		}
	    }
	}
	else {
	    if (rb_obj_is_kind_of(current_exception, obj)) {
		active = 1;
	    }
	}
    }
    va_end(ar);

    return active;
}

extern "C"
void
rb_vm_pop_exception(void)
{
    GET_VM()->pop_current_exception();
}

extern "C"
VALUE
rb_vm_current_exception(void)
{
    return GET_VM()->current_exception();
}

extern "C"
void
rb_vm_set_current_exception(VALUE exception)
{
    assert(!NIL_P(exception));

    VALUE current = GET_VM()->current_exception();
    assert(exception != current);
    if (!NIL_P(current)) {
	GET_VM()->pop_current_exception();
    }
    GET_VM()->push_current_exception(exception);
}

extern "C"
void 
rb_vm_debug(void)
{
    printf("rb_vm_debug\n");
}

extern "C"
void
rb_vm_print_current_exception(void)
{
    VALUE exc = GET_VM()->current_exception();
    if (exc == Qnil) {
	printf("uncaught Objective-C/C++ exception...\n");
	std::terminate();
    }

    VALUE message = rb_vm_call(exc, sel_registerName("message"), 0, NULL,
	    false);
    VALUE bt = rb_vm_call(exc, sel_registerName("backtrace"), 0, NULL,
	    false);

    const long count = RARRAY_LEN(bt);
    if (count > 0) {
	for (long i = 0; i < count; i++) {
	    const char *bte = RSTRING_PTR(RARRAY_AT(bt, i));
	    if (i == 0) {
		printf("%s: %s (%s)\n", bte, RSTRING_PTR(message),
			rb_class2name(*(VALUE *)exc));
	    }
	    else {
		printf("\tfrom %s\n", bte);
	    }
	}
    }
    else {
	printf("%s (%s)\n", RSTRING_PTR(message),
		rb_class2name(*(VALUE *)exc));
    }
}

extern "C"
bool
rb_vm_parse_in_eval(void)
{
    return GET_VM()->get_parse_in_eval();
}

extern "C"
void
rb_vm_set_parse_in_eval(bool flag)
{
    GET_VM()->set_parse_in_eval(flag);
}

extern "C"
int
rb_parse_in_eval(void)
{
    return rb_vm_parse_in_eval() ? 1 : 0;
}

VALUE *
RoxorVM::get_binding_lvar(ID name, bool create)
{
    if (!bindings.empty()) {
	rb_vm_binding_t *b = bindings.back();
	rb_vm_local_t **l = &b->locals;
	while (*l != NULL) {
	    if ((*l)->name == name) {
		return (*l)->value;
	    }
	    l = &(*l)->next;
	}
	if (create) {
	    GC_WB(l, xmalloc(sizeof(rb_vm_local_t)));
	    (*l)->name = name;
	    GC_WB(&(*l)->value, xmalloc(sizeof(VALUE)));
	    (*l)->next = NULL;
	    return (*l)->value;
	}
    }
    return NULL;
}

extern "C"
int
rb_local_define(ID id)
{
    return GET_VM()->get_binding_lvar(id, true) != NULL ? 1 : 0;
}

extern "C"
int
rb_local_defined(ID id)
{
    return GET_VM()->get_binding_lvar(id, false) != NULL ? 1 : 0;
}

extern "C"
int
rb_dvar_defined(ID id)
{
    // TODO
    return 0;
}

extern "C"
void
rb_vm_init_compiler(void)
{
    RoxorCompiler::shared = ruby_aot_compile
	? new RoxorAOTCompiler()
	: new RoxorCompiler();
}

extern "C" void rb_node_release(NODE *node);

extern "C"
VALUE
rb_vm_run(const char *fname, NODE *node, rb_vm_binding_t *binding,
	  bool inside_eval)
{
    RoxorVM *vm = GET_VM();
    RoxorCompiler *compiler = RoxorCompiler::shared;

    // Compile IR.
    if (binding != NULL) {
	vm->push_current_binding(binding, false);
    }
    bool old_inside_eval = compiler->is_inside_eval();
    compiler->set_inside_eval(inside_eval);
    compiler->set_fname(fname);
    Function *function = compiler->compile_main_function(node);
    compiler->set_fname(NULL);
    compiler->set_inside_eval(old_inside_eval);
    if (binding != NULL) {
	vm->pop_current_binding(false);
    }

    // JIT compile the function.
    IMP imp = GET_CORE()->compile(function);

    // Register it for symbolication.
    rb_vm_method_node_t *mnode = GET_CORE()->method_node_get(imp, true);
    mnode->klass = 0;
    mnode->arity = rb_vm_arity(2);
    mnode->sel = sel_registerName("<main>");
    mnode->objc_imp = mnode->ruby_imp = imp;
    mnode->flags = 0;

    // Execute the function.
    VALUE ret = ((VALUE(*)(VALUE, SEL))imp)(vm->get_current_top_object(), 0);

    if (inside_eval) {
	// XXX We only delete functions created by #eval. In theory it should
	// also work for other functions, but it makes spec:ci crash.
	GET_CORE()->delenda(function);
    }

    rb_node_release(node);

    return ret;
}

extern "C"
VALUE
rb_vm_run_under(VALUE klass, VALUE self, const char *fname, NODE *node,
	rb_vm_binding_t *binding, bool inside_eval)
{
    RoxorVM *vm = GET_VM();

    VALUE old_top_object = vm->get_current_top_object();
    if (binding != NULL) {
	self = binding->self;
    }
    if (self != 0) {
	vm->set_current_top_object(self);
    }
    Class old_class = GET_VM()->get_current_class();
    bool old_dynamic_class = RoxorCompiler::shared->is_dynamic_class();
    vm->set_current_class((Class)klass);
    RoxorCompiler::shared->set_dynamic_class(true);

    vm->add_current_block(binding != NULL ? binding->block : NULL);

    struct Finally {
	RoxorVM *vm;
	bool old_dynamic_class;
	Class old_class;
	VALUE old_top_object;
	Finally(RoxorVM *_vm, bool _dynamic_class, Class _class, VALUE _obj) {
	    vm = _vm;
	    old_dynamic_class = _dynamic_class;
	    old_class = _class;
	    old_top_object = _obj;
	}
	~Finally() { 
	    RoxorCompiler::shared->set_dynamic_class(old_dynamic_class);
	    vm->set_current_top_object(old_top_object);
	    vm->set_current_class(old_class);
	    vm->pop_current_block();
	}
    } finalizer(vm, old_dynamic_class, old_class, old_top_object);

    return rb_vm_run(fname, node, binding, inside_eval);
}

extern "C"
void
rb_vm_aot_compile(NODE *node)
{
    assert(ruby_aot_compile);
    assert(ruby_aot_init_func);

    // Compile the program as IR.
    Function *f = RoxorCompiler::shared->compile_main_function(node);
    f->setName(RSTRING_PTR(ruby_aot_init_func));
    GET_CORE()->optimize(f);

    // Force a module verification.
    if (verifyModule(*RoxorCompiler::module, PrintMessageAction)) {
	printf("Error during module verification\n");
	exit(1);
    }

    // Dump the bitcode.
    std::string err;
    const char *output = RSTRING_PTR(ruby_aot_compile);
    raw_fd_ostream out(output, err, raw_fd_ostream::F_Binary);
    if (!err.empty()) {
	fprintf(stderr, "error when opening the output bitcode file: %s\n",
		err.c_str());
	abort();
    }
    WriteBitcodeToFile(RoxorCompiler::module, out);
    out.close();
}

extern "C"
VALUE
rb_vm_top_self(void)
{
    return GET_VM()->get_current_top_object();
}

extern "C"
VALUE
rb_vm_loaded_features(void)
{
    return GET_CORE()->get_loaded_features();
}

extern "C"
VALUE
rb_vm_load_path(void)
{
    return GET_CORE()->get_load_path();
}

extern "C"
int
rb_vm_safe_level(void)
{
    return GET_VM()->get_safe_level();
}

extern "C"
int
rb_vm_thread_safe_level(rb_vm_thread_t *thread)
{
    return ((RoxorVM *)thread->vm)->get_safe_level();
}

extern "C"
void 
rb_vm_set_safe_level(int level)
{
    GET_VM()->set_safe_level(level);
}

extern "C"
VALUE
rb_last_status_get(void)
{
    return GET_VM()->get_last_status();
}

extern "C"
void
rb_last_status_set(int status, rb_pid_t pid)
{
    VALUE last_status = GET_VM()->get_last_status();
    if (last_status != Qnil) {
	GC_RELEASE(last_status);
    }

    if (pid == -1) {
	last_status = Qnil;
    }
    else {
	last_status = rb_obj_alloc(rb_cProcessStatus);
	rb_iv_set(last_status, "status", INT2FIX(status));
	rb_iv_set(last_status, "pid", PIDT2NUM(pid));
	GC_RETAIN(last_status);
    }
    GET_VM()->set_last_status(last_status);
}

extern "C"
VALUE
rb_errinfo(void)
{
    return GET_VM()->get_errinfo();
}

void
rb_set_errinfo(VALUE err)
{
    if (!NIL_P(err) && !rb_obj_is_kind_of(err, rb_eException)) {
        rb_raise(rb_eTypeError, "assigning non-exception to $!");
    }
    VALUE errinfo = GET_VM()->get_errinfo();
    if (errinfo != Qnil) {
	GC_RELEASE(errinfo);
    }
    GET_VM()->set_errinfo(err);
    GC_RETAIN(err);
}

extern "C"
const char *
rb_sourcefile(void)
{
    // TODO
    return "unknown";
}

extern "C"
int
rb_sourceline(void)
{
    // TODO
    return 0;
}

extern "C"
VALUE
rb_lastline_get(void)
{
    // TODO
    return Qnil;
}

extern "C"
void
rb_lastline_set(VALUE val)
{
    // TODO
}

extern "C"
void
rb_iter_break(void)
{
    GET_VM()->set_broken_with(Qnil);
}

extern "C"
VALUE
rb_backref_get(void)
{
    return GET_VM()->get_backref();
}

extern "C"
void
rb_backref_set(VALUE val)
{
    VALUE old = GET_VM()->get_backref();
    if (old != val) {
	GC_RELEASE(old);
	GET_VM()->set_backref(val);
	GC_RETAIN(val);
    }
}

VALUE
RoxorVM::ruby_catch(VALUE tag)
{
    std::map<VALUE, int*>::iterator iter = catch_nesting.find(tag);

    int *nested_ptr = NULL;
    if (iter == catch_nesting.end()) {
	nested_ptr = (int *)malloc(sizeof(int));
	*nested_ptr = 1;
	catch_nesting[tag] = nested_ptr;
	GC_RETAIN(tag);
    }
    else {
	nested_ptr = iter->second;
	(*nested_ptr)++;
    }

    VALUE retval = Qundef;
    try {
	retval = rb_vm_yield(1, &tag);
    }
    catch (...) {
	// Cannot catch "RoxorCatchThrowException *exc", otherwise the program
	// will crash when trying to interpret ruby exceptions.
	// So we catch ... instead, and retrieve the exception from the VM.
	std::type_info *t = __cxa_current_exception_type();
	if (t != NULL && *t == typeid(RoxorCatchThrowException *)) {
	    RoxorCatchThrowException *exc = GET_VM()->get_throw_exc();
	    if (exc != NULL && exc->throw_symbol == tag) {
		retval = exc->throw_value;
		rb_objc_release((void *)retval);
		delete exc;
		GET_VM()->set_throw_exc(NULL);
	    }
	}
	if (retval == Qundef) {
	    throw;
	}
    }

    iter = catch_nesting.find(tag);
    assert(iter != catch_nesting.end());
    (*nested_ptr)--;
    if (*nested_ptr == 0) {
	nested_ptr = iter->second;
	free(nested_ptr);
	catch_nesting.erase(iter);
	GC_RELEASE(tag);
    }

    return retval;
}

extern "C"
VALUE
rb_vm_catch(VALUE tag)
{
    return GET_VM()->ruby_catch(tag);
}

VALUE
RoxorVM::ruby_throw(VALUE tag, VALUE value)
{
    std::map<VALUE, int*>::iterator iter = catch_nesting.find(tag);

    if (iter == catch_nesting.end()) {
        VALUE desc = rb_inspect(tag);
        rb_raise(rb_eArgError, "uncaught throw %s", RSTRING_PTR(desc));
    }

    rb_objc_retain((void *)value);

    RoxorCatchThrowException *exc = new RoxorCatchThrowException;
    exc->throw_symbol = tag;
    exc->throw_value = value;
    // There is no way - yet - to retrieve the exception from the ABI
    // So instead, we store the exception in the VM
    GET_VM()->set_throw_exc(exc);
    throw exc;

    return Qnil; // Never reached;
}

extern "C"
VALUE
rb_vm_throw(VALUE tag, VALUE value)
{
    return GET_VM()->ruby_throw(tag, value);
}

extern "C"
VALUE
rb_exec_recursive(VALUE (*func) (VALUE, VALUE, int), VALUE obj, VALUE arg)
{
    return GET_VM()->exec_recursive(func, obj, arg);
}

VALUE
RoxorVM::exec_recursive(VALUE (*func) (VALUE, VALUE, int), VALUE obj,
	VALUE arg)
{
    std::vector<VALUE>::iterator iter =
	std::find(recursive_objects.begin(), recursive_objects.end(), obj);
    if (iter != recursive_objects.end()) {
	// Object is already being iterated!
	return (*func) (obj, arg, Qtrue);
    }

    recursive_objects.push_back(obj);
    // XXX the function is not supposed to raise an exception.
    VALUE ret = (*func) (obj, arg, Qfalse);

    iter = std::find(recursive_objects.begin(), recursive_objects.end(), obj);
    assert(iter != recursive_objects.end());
    recursive_objects.erase(iter);

    return ret;
}

extern "C"
void
rb_vm_register_finalizer(rb_vm_finalizer_t *finalizer)
{
    GET_CORE()->register_finalizer(finalizer);
}

extern "C"
void
rb_vm_unregister_finalizer(rb_vm_finalizer_t *finalizer)
{
    GET_CORE()->unregister_finalizer(finalizer);
}

void
RoxorCore::register_finalizer(rb_vm_finalizer_t *finalizer)
{
    lock();
    finalizers.push_back(finalizer);
    unlock();
}

void
RoxorCore::unregister_finalizer(rb_vm_finalizer_t *finalizer)
{
    lock();
    std::vector<rb_vm_finalizer_t *>::iterator i = std::find(finalizers.begin(),
	    finalizers.end(), finalizer);
    if (i != finalizers.end()) {
	finalizers.erase(i);
    }
    unlock();
}

static void
call_finalizer(rb_vm_finalizer_t *finalizer)
{
    for (int i = 0, count = RARRAY_LEN(finalizer->finalizers); i < count; i++) {
	VALUE b = RARRAY_AT(finalizer->finalizers, i);
	try {
	    rb_vm_call(b, selCall, 1, &finalizer->objid, false);
	}
	catch (...) {
	    // Do nothing.
	}
    }
    rb_ary_clear(finalizer->finalizers);
}

extern "C"
void
rb_vm_call_finalizer(rb_vm_finalizer_t *finalizer)
{
    call_finalizer(finalizer);
}

void
RoxorCore::call_all_finalizers(void)
{
    for (std::vector<rb_vm_finalizer_t *>::iterator i = finalizers.begin();
	    i != finalizers.end();
	    ++i) {
	call_finalizer(*i);
    }
    finalizers.clear();
}

extern "C"
void *
rb_vm_create_vm(void)
{
    GET_CORE()->set_multithreaded(true);

    return (void *)new RoxorVM(*GET_VM());
}

extern "C"
bool
rb_vm_is_multithreaded(void)
{
    return GET_CORE()->get_multithreaded();
}

extern "C"
void
rb_vm_set_multithreaded(bool flag)
{
    GET_CORE()->set_multithreaded(flag);
}

void
RoxorCore::register_thread(VALUE thread)
{
    lock();
    rb_ary_push(threads, thread);
    unlock();

    rb_vm_thread_t *t = GetThreadPtr(thread);
    pthread_assert(pthread_setspecific(RoxorVM::vm_thread_key, t->vm));

    RoxorVM *vm = (RoxorVM *)t->vm;
    vm->set_thread(thread);
}

extern "C" void rb_thread_unlock_all_mutexes(rb_vm_thread_t *thread);

void
RoxorCore::unregister_thread(VALUE thread)
{
    lock();
    if (rb_ary_delete(threads, thread) != thread) {
	printf("trying to unregister a thread (%p) that was never registered!",
		(void *)thread);
	abort();
    }
    unlock();

    rb_vm_thread_t *t = GetThreadPtr(thread);

    const int code = pthread_mutex_destroy(&t->sleep_mutex);
    if (code == EBUSY) {
	// The mutex is already locked, which means we are being called from
	// a cancellation point inside the wait logic. Let's unlock the mutex
	// and try again.
	pthread_assert(pthread_mutex_unlock(&t->sleep_mutex));
	pthread_assert(pthread_mutex_destroy(&t->sleep_mutex));
    }
    else if (code != 0) {
	abort();
    }
    pthread_assert(pthread_cond_destroy(&t->sleep_cond));

    rb_thread_unlock_all_mutexes(t); 

    RoxorVM *vm = (RoxorVM *)t->vm;
    delete vm;
    t->vm = NULL;

    pthread_assert(pthread_setspecific(RoxorVM::vm_thread_key, NULL));

    t->status = THREAD_DEAD;
}

static inline void
rb_vm_thread_throw_kill(void)
{
    // Killing a thread is implemented using a non-catchable (from Ruby)
    // exception, which allows us to call the ensure blocks before dying,
    // which is unfortunately covered in the Ruby specifications.
    rb_vm_rethrow();
}

static void
rb_vm_thread_destructor(void *userdata)
{
    rb_vm_thread_throw_kill();
}

extern "C"
void *
rb_vm_thread_run(VALUE thread)
{
    rb_objc_gc_register_thread();
    GET_CORE()->register_thread(thread);

    // Release the thread now.
    GC_RELEASE(thread);

    rb_vm_thread_t *t = GetThreadPtr(thread);

    // Normally the pthread ID is set into the VM structure in the other
    // thread right after pthread_create(), but we might run before the
    // assignment!
    t->thread = pthread_self();

    pthread_cleanup_push(rb_vm_thread_destructor, (void *)thread);

    try {
	VALUE val = rb_vm_block_eval(t->body, t->argc, t->argv);
	GC_WB(&t->value, val);
    }
    catch (...) {
	VALUE exc;
	if (current_exception_is_return_from_block()) {
	    // TODO: the exception is leaking!
	    exc = rb_exc_new2(rb_eLocalJumpError,
		    "unexpected return from Thread");
	}
	else {
	    exc = rb_vm_current_exception();
	}
	if (exc != Qnil) {
	    GC_WB(&t->exception, exc);
	}
	t->value = Qnil;
    }

    pthread_cleanup_pop(0);

    rb_thread_remove_from_group(thread); 
    GET_CORE()->unregister_thread(thread);
    rb_objc_gc_unregister_thread();

#if 0
    if (t->exception != Qnil) {
	if (t->abort_on_exception || GET_CORE()->get_abort_on_exception()) {
 	    // TODO: move the exception to the main thread
	    //rb_exc_raise(t->exception);
	}
    }
#endif

    return NULL;
}

extern "C"
VALUE
rb_vm_threads(void)
{
    return GET_CORE()->get_threads();
}

extern "C"
VALUE
rb_vm_current_thread(void)
{
    return GET_VM()->get_thread();
}

extern "C"
VALUE
rb_thread_current(void)
{
    // For compatibility with MRI 1.9.
    return rb_vm_current_thread();
}

extern "C"
VALUE
rb_vm_main_thread(void)
{
    return RoxorVM::main->get_thread();
}

extern "C"
VALUE
rb_vm_thread_locals(VALUE thread, bool create_storage)
{
    rb_vm_thread_t *t = GetThreadPtr(thread);
    if (t->locals == Qnil && create_storage) {
	GC_WB(&t->locals, rb_hash_new());
    }
    return t->locals;
}

extern "C"
void
rb_vm_thread_pre_init(rb_vm_thread_t *t, rb_vm_block_t *body, int argc,
	const VALUE *argv, void *vm)
{
    t->thread = 0; // this will be set later

    if (body != NULL) {
	GC_WB(&t->body, body);
	rb_vm_block_make_detachable_proc(body);
    }
    else {
	t->body = NULL;
    }
   
    if (argc > 0) {
	t->argc = argc;
	GC_WB(&t->argv, xmalloc(sizeof(VALUE) * argc));
	for (int i = 0; i < argc; i++) {
	    GC_WB(&t->argv[i], argv[i]);
	}
    }
    else {
	t->argc = 0;
	t->argv = NULL;
    }

    t->vm  = vm;
    t->value = Qundef;
    t->locals = Qnil;
    t->exception = Qnil;
    t->status = THREAD_ALIVE;
    t->in_cond_wait = false;
    t->abort_on_exception = false;
    t->group = Qnil; // will be set right after
    t->mutexes = Qnil;

    pthread_assert(pthread_mutex_init(&t->sleep_mutex, NULL));
    pthread_assert(pthread_cond_init(&t->sleep_cond, NULL)); 
}

static inline void
pre_wait(rb_vm_thread_t *t)
{
    pthread_assert(pthread_mutex_lock(&t->sleep_mutex));
    t->status = THREAD_SLEEP;
    t->in_cond_wait = true;
}

static inline void
post_wait(rb_vm_thread_t *t)
{
    t->in_cond_wait = false;
    pthread_assert(pthread_mutex_unlock(&t->sleep_mutex));
    if (t->status == THREAD_KILLED) {
	rb_vm_thread_throw_kill();
    }
    t->status = THREAD_ALIVE;
}

extern "C"
void
rb_thread_sleep_forever()
{
    rb_vm_thread_t *t = GET_THREAD();

    pre_wait(t);
    const int code = pthread_cond_wait(&t->sleep_cond, &t->sleep_mutex);
    assert(code == 0 || code == ETIMEDOUT);
    post_wait(t);
}

extern "C"
void
rb_thread_wait_for(struct timeval time)
{
    struct timeval tvn;
    gettimeofday(&tvn, NULL);

    struct timespec ts;
    ts.tv_sec = tvn.tv_sec + time.tv_sec;
    ts.tv_nsec = (tvn.tv_usec + time.tv_usec) * 1000;
    while (ts.tv_nsec >= 1000000000) {
	ts.tv_sec += 1;
	ts.tv_nsec -= 1000000000;
    }

    rb_vm_thread_t *t = GET_THREAD();

    pre_wait(t);
    const int code = pthread_cond_timedwait(&t->sleep_cond, &t->sleep_mutex,
	    &ts);
    assert(code == 0 || code == ETIMEDOUT);
    post_wait(t);
}

extern "C"
void
rb_vm_thread_wakeup(rb_vm_thread_t *t)
{
    if (t->status == THREAD_DEAD) {
	rb_raise(rb_eThreadError, "can't wake up thread from the death");
    }
    if (t->status == THREAD_SLEEP && t->in_cond_wait) {
	pthread_assert(pthread_mutex_lock(&t->sleep_mutex));
	pthread_assert(pthread_cond_signal(&t->sleep_cond));
	pthread_assert(pthread_mutex_unlock(&t->sleep_mutex));
    }
}

extern "C"
void
rb_vm_thread_cancel(rb_vm_thread_t *t)
{
    if (t->status != THREAD_KILLED && t->status != THREAD_DEAD) {
	t->status = THREAD_KILLED;
	if (t->thread == pthread_self()) {
	    rb_vm_thread_throw_kill();
	}
	else {
	    pthread_assert(pthread_mutex_lock(&t->sleep_mutex));
	    if (t->in_cond_wait) {
		// We are trying to kill a thread which is currently waiting
		// for a condition variable (#sleep). Instead of canceling the
		// thread, we are simply signaling the variable, and the thread
		// will autodestroy itself, to work around a stack unwinding
		// bug in the Mac OS X pthread implementation that messes our
		// C++ exception handlers.
		pthread_assert(pthread_cond_signal(&t->sleep_cond));
	    }
	    else {
		pthread_assert(pthread_cancel(t->thread));
	    }
	    pthread_assert(pthread_mutex_unlock(&t->sleep_mutex));
	}
    }
}

extern "C"
void
rb_vm_thread_raise(rb_vm_thread_t *t, VALUE exc)
{
    // XXX we should lock here
    RoxorVM *vm = (RoxorVM *)t->vm;
    vm->push_current_exception(exc);

    rb_vm_thread_cancel(t);
}

extern "C"
void
rb_thread_sleep(int sec)
{
    struct timeval time;
    time.tv_sec = sec;
    time.tv_usec = 0;
    rb_thread_wait_for(time);
}

extern "C"
Class
rb_vm_set_current_class(Class klass)
{
    RoxorVM *vm = GET_VM();
    Class old = vm->get_current_class();
    vm->set_current_class(klass);
    return old;
}

extern "C"
Class
rb_vm_get_current_class(void)
{
    return GET_VM()->get_current_class();
}

extern "C"
void
rb_vm_set_current_scope(VALUE mod, rb_vm_scope_t scope)
{
    if (scope == SCOPE_DEFAULT) {
	scope = mod == rb_cObject ? SCOPE_PRIVATE : SCOPE_PUBLIC;
    }
    long v = RCLASS_VERSION(mod);
#if ROXOR_VM_DEBUG
    const char *scope_name = NULL;
#endif
    switch (scope) {
	case SCOPE_PUBLIC:
#if ROXOR_VM_DEBUG
	    scope_name = "public";
#endif
	    v &= ~RCLASS_SCOPE_PRIVATE;
	    v &= ~RCLASS_SCOPE_PROTECTED;
	    v &= ~RCLASS_SCOPE_MOD_FUNC;
	    break;

	case SCOPE_PRIVATE:
#if ROXOR_VM_DEBUG
	    scope_name = "private";
#endif
	    v |= RCLASS_SCOPE_PRIVATE;
	    v &= ~RCLASS_SCOPE_PROTECTED;
	    v &= ~RCLASS_SCOPE_MOD_FUNC;
	    break;

	case SCOPE_PROTECTED:
#if ROXOR_VM_DEBUG
	    scope_name = "protected";
#endif
	    v &= ~RCLASS_SCOPE_PRIVATE;
	    v |= RCLASS_SCOPE_PROTECTED;
	    v &= ~RCLASS_SCOPE_MOD_FUNC;
	    break;

	case SCOPE_MODULE_FUNC:
#if ROXOR_VM_DEBUG
	    scope_name = "module_func";
#endif
	    v &= ~RCLASS_SCOPE_PRIVATE;
	    v &= ~RCLASS_SCOPE_PROTECTED;
	    v |= RCLASS_SCOPE_MOD_FUNC;
	    break;

	case SCOPE_DEFAULT:
	    abort(); // handled earlier
    }

#if ROXOR_VM_DEBUG
    printf("changing scope of %s (%p) to %s\n",
	    class_getName((Class)mod), (void *)mod, scope_name);
#endif

    RCLASS_SET_VERSION(mod, v);
}

static VALUE
builtin_ostub1(IMP imp, id self, SEL sel, int argc, VALUE *argv)
{
    return OC2RB(((id (*)(id, SEL))*imp)(self, sel));
}

static void
setup_builtin_stubs(void)
{
    GET_CORE()->insert_stub("@@:", (void *)builtin_ostub1, true);
    GET_CORE()->insert_stub("#@:", (void *)builtin_ostub1, true);
}

static IMP old_resolveClassMethod_imp = NULL;
static IMP old_resolveInstanceMethod_imp = NULL;

static BOOL
resolveClassMethod_imp(void *self, SEL sel, SEL name)
{
    if (rb_vm_resolve_method(*(Class *)self, name)) {
	return YES;
    }
    return NO; // TODO call old IMP
}

static BOOL
resolveInstanceMethod_imp(void *self, SEL sel, SEL name)
{
    if (rb_vm_resolve_method((Class)self, name)) {
	return YES;
    }
    return NO; // TODO call old IMP
}

// We can't trust LLVM to pick the right target at runtime.
#if __LP64__
# define TARGET_TRIPLE "x86_64-apple-darwin"
#else
# define TARGET_TRIPLE "i386-apple-darwin"
#endif

extern "C"
void 
Init_PreVM(void)
{
    llvm::DwarfExceptionHandling = true; // required!

    RoxorCompiler::module = new llvm::Module("Roxor", getGlobalContext());
    RoxorCompiler::module->setTargetTriple(TARGET_TRIPLE);
    RoxorCore::shared = new RoxorCore();
    RoxorVM::main = new RoxorVM();

    pthread_assert(pthread_key_create(&RoxorVM::vm_thread_key, NULL));

    setup_builtin_stubs();

    Method m;
    Class ns_object = (Class)objc_getClass("NSObject");
    m = class_getInstanceMethod(*(Class *)ns_object,
	sel_registerName("resolveClassMethod:"));
    assert(m != NULL);
    old_resolveClassMethod_imp = method_getImplementation(m);
    method_setImplementation(m, (IMP)resolveClassMethod_imp);

    m = class_getInstanceMethod(*(Class *)ns_object,
	sel_registerName("resolveInstanceMethod:"));
    assert(m != NULL);
    old_resolveInstanceMethod_imp = method_getImplementation(m);
    method_setImplementation(m, (IMP)resolveInstanceMethod_imp);
}

static VALUE
rb_toplevel_to_s(VALUE rcv, SEL sel)
{
    return rb_str_new2("main");
}

static const char *
resources_path(char *path, size_t len)
{
    CFBundleRef bundle;
    CFURLRef url;

    bundle = CFBundleGetMainBundle();
    assert(bundle != NULL);

    url = CFBundleCopyResourcesDirectoryURL(bundle);
    *path = '-'; 
    *(path+1) = 'I';
    assert(CFURLGetFileSystemRepresentation(
		url, true, (UInt8 *)&path[2], len - 2));
    CFRelease(url);

    return path;
}

extern "C"
int
macruby_main(const char *path, int argc, char **argv)
{
    char **newargv;
    char *p1, *p2;
    int n, i;

    newargv = (char **)malloc(sizeof(char *) * (argc + 2));
    for (i = n = 0; i < argc; i++) {
	if (!strncmp(argv[i], "-psn_", 5) == 0) {
	    newargv[n++] = argv[i];
	}
    }
    
    p1 = (char *)malloc(PATH_MAX);
    newargv[n++] = (char *)resources_path(p1, PATH_MAX);

    p2 = (char *)malloc(PATH_MAX);
    snprintf(p2, PATH_MAX, "%s/%s", (path[0] != '/') ? &p1[2] : "", path);
    newargv[n++] = p2;

    argv = newargv;    
    argc = n;

    try {
	ruby_sysinit(&argc, &argv);
	ruby_init();
	void *tree = ruby_options(argc, argv);
	rb_vm_init_compiler();
	free(newargv);
	free(p1);
	free(p2);
	return ruby_run_node(tree);
    }
    catch (...) {
	rb_vm_print_current_exception();
	exit(1);	
    }
}

extern "C"
void
Init_VM(void)
{
    rb_cTopLevel = rb_define_class("TopLevel", rb_cObject);
    rb_objc_define_method(rb_cTopLevel, "to_s", (void *)rb_toplevel_to_s, 0);

    GET_VM()->set_current_class(NULL);

    VALUE top_self = rb_obj_alloc(rb_cTopLevel);
    rb_objc_retain((void *)top_self);
    GET_VM()->set_current_top_object(top_self);

    rb_vm_set_current_scope(rb_cNSObject, SCOPE_PRIVATE);
}

void
RoxorVM::setup_from_current_thread(void)
{
    pthread_setspecific(RoxorVM::vm_thread_key, (void *)this);

    rb_vm_thread_t *t = (rb_vm_thread_t *)xmalloc(sizeof(rb_vm_thread_t));
    rb_vm_thread_pre_init(t, NULL, 0, NULL, (void *)this);
    t->thread = pthread_self();

    VALUE thread = Data_Wrap_Struct(rb_cThread, NULL, NULL, t);
    GET_CORE()->register_thread(thread);
    this->set_thread(thread);
}

extern "C"
void
rb_vm_register_current_alien_thread(void)
{
    // This callback is not used, we prefer to create RoxorVM objects
    // lazily (in RoxorVM::current()), for performance reasons, because the
    // callback is called *a lot* and most of the time from various parts of
    // the system which will never ask us to execute Ruby code.
#if 0
    if (GET_CORE()->get_running()) {
	printf("registered alien thread %p\n", pthread_self());
	RoxorVM *vm = new RoxorVM();
	vm->setup_from_current_thread();
    }
#endif
}

extern "C"
void
rb_vm_unregister_current_alien_thread(void)
{
    // Check if the current pthread has been registered.
    GET_CORE()->lock();
    pthread_t self = pthread_self();
    VALUE ary = GET_CORE()->get_threads();
    bool need_to_unregister = false;
    for (int i = 0; i < RARRAY_LEN(ary); i++) {
	VALUE t = RARRAY_AT(ary, i);
	if (GetThreadPtr(t)->thread == self) {
	    need_to_unregister = true;
	}
    }
    GET_CORE()->unlock();

    // If yes, appropriately unregister it.
    if (need_to_unregister) {
	//printf("unregistered alien thread %p\n", pthread_self());
	GET_CORE()->unregister_thread(GET_VM()->get_thread());
    }
}

extern "C"
void
Init_PostVM(void)
{
    // Create and register the main thread.
    RoxorVM *main_vm = GET_VM();
    main_vm->setup_from_current_thread();

    // Create main thread group.
    VALUE group = rb_obj_alloc(rb_cThGroup);
    rb_thgroup_add(group, main_vm->get_thread());
    rb_define_const(rb_cThGroup, "Default", group);
}

extern "C"
void
rb_vm_finalize(void)
{
    if (getenv("VM_DUMP_IR") != NULL) {
	printf("IR dump ----------------------------------------------\n");
	RoxorCompiler::module->dump();
	printf("------------------------------------------------------\n");
    }
#if ROXOR_VM_DEBUG
    printf("functions all=%ld compiled=%ld\n", RoxorCompiler::module->size(),
	    GET_CORE()->get_functions_compiled());
#endif


    // XXX: deleting the core is not safe at this point because there might be
    // threads still running and trying to unregister.
//    delete RoxorCore::shared;
//    RoxorCore::shared = NULL;
    GET_CORE()->call_all_finalizers();
}
