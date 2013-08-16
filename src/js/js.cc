/*
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "js.h"

#include <pthread.h> /* for V8_HACK_RUN_TIME */

#include <say.h>
#include <fiber.h>

#include "require.h"
#include "platform.h"
#include "stub.h"
#include "fiber.h"

/*
 * The v8 TLS hack
 * The hack replaces pthread's local storage with our fiber local storage
 */

#if defined(ENABLE_V8_HACK_LINK_TIME)
/* Use link-time version of the hack */

/* It is known that the link-time version does not work if v8 was compiled with
 * visibility=hidden */

static volatile bool V8_THREAD_LOCAL_HACK_WORKS = false;

/* Overload some v8 internal symbols in this compilation unit */
#include "v8_tls_hack_link_time.cc"

static void
js_tls_hack_init(void)
{
	if (!v8::V8::Initialize())
		goto error_1;

	if (V8_THREAD_LOCAL_HACK_WORKS) {
		say_debug("v8 TLS link-time hack works");
		return;
	}

error_1:
	panic("v8 TLS link-time hack does not work - "
	      "try to recompile without -DENABLE_V8_HACK_LINK_TIME=0");

	return;
}

#else /* !defined(ENABLE_V8_HACK_LINK_TIME) */

/* See fiber.h for V8_TLS_SIZE */
static pthread_key_t js_tls_pthread_keys[V8_TLS_SIZE];

static void
js_tls_hack_init(void)
{
	pthread_key_t last_key;
	size_t js_tls_size = 0;
	v8::Isolate *isolate;

	if (pthread_key_create(&last_key, NULL) != 0)
		goto error_1;

	enum { FLS_CHECK_MAX = 1024 };

	if (last_key > FLS_CHECK_MAX)
		goto error_2;

	void *fls[FLS_CHECK_MAX];
	for (pthread_key_t k = 0; k < last_key; k++) {
		fls[k] = pthread_getspecific(k);
	}

	/* Initialize v8 and see what is changed */
	if (!v8::V8::Initialize())
		goto error_2;

	isolate = v8::Isolate::GetCurrent();
	if (isolate == NULL)
		goto error_2;

	for (pthread_key_t k = 0; k < last_key; k++) {
		void *cur = pthread_getspecific(k);
		/*
		 * Check if the key was changed or if the key contains
		 * a pointer to the current v8::Isolate which could be created
		 * before our hack
		 */
		if (cur != fls[k] || cur == isolate) {
			js_tls_pthread_keys[js_tls_size++] = k;
		}
	}

	/* Check if we found enough number of keys */
	if (js_tls_size != V8_TLS_SIZE)
		goto error_2;

	say_debug("v8 TLS run-time hack works - "
		  "all %zu keys have been detected", js_tls_size);
	say_debug("v8 pthread's TLS <-> fiber's FLS mapping:");
	for (size_t t = 0; t < V8_TLS_SIZE; t++) {
		say_debug(" %u => %zu",
			  (uint32_t) js_tls_pthread_keys[t], t);
	}

	pthread_key_delete(last_key);

	return;

error_2:
	pthread_key_delete(last_key);
error_1:
	panic("Can not detect v8 TLS keys - the hack does not work :(");
}

/**
 * @brief Clear all v8 pthread's TLS data
 */
static void
js_tls_hack_pthread_clear(void)
{
	for (size_t t = 0; t < V8_TLS_SIZE; t++) {
		pthread_setspecific(js_tls_pthread_keys[t], NULL);
	}
}

/**
 * @brief Copy v8 pthread's TLS data to fiber's FLS
 */
static void
js_tls_hack_pthread_to_fiber(void)
{
	for (size_t t = 0; t < V8_TLS_SIZE; t++) {
		void *var = pthread_getspecific(js_tls_pthread_keys[t]);
		fiber->js_tls[t] = var;
	}
}

/**
 * @brief Copy v8 fiber's FLS data to pthread's TLS
 */
static void
js_tls_hack_fiber_to_pthread(void)
{
	for (size_t t = 0; t < V8_TLS_SIZE; t++) {
		pthread_setspecific(js_tls_pthread_keys[t], fiber->js_tls[t]);
	}
}

#endif /* defined(ENABLE_V8_HACK_LINK_TIME) */

namespace { /* anonymous */

#define mh_name _tmplcache
#define mh_key_t intptr_t
struct mh_tmplcache_node_t {
	mh_key_t key;
	void *val;
};

#define mh_node_t struct mh_tmplcache_node_t
#define mh_arg_t void *
#define mh_hash(a, arg) (a->key)
#define mh_hash_key(a, arg) (a)
#define mh_eq(a, b, arg) ((a->key) == (b->key))
#define mh_eq_key(a, b, arg) ((a) == (b->key))
#define MH_SOURCE 1
#include <mhash.h>

struct ev_idle idle_watcher;

class ArrayBufferAllocator: public v8::ArrayBuffer::Allocator {
public:
	virtual void* Allocate(size_t length)
	{
		return calloc(1, length);
	}

	virtual void* AllocateUninitialized(size_t length)
	{
		return malloc(length);
	}

	virtual void Free(void* data, size_t)
	{
		free(data);
	}
};

static ArrayBufferAllocator array_buffer_allocator;

static inline void
V8SetFlag(const char *flag)
{
	v8::V8::SetFlagsFromString(flag, strlen(flag));
}


void
V8Idle(EV_P_ struct ev_idle *w, int revents)
{
	(void) revents;
	(void) w;

	say_debug("js idle");
	if (v8::V8::IdleNotification()) {
		ev_idle_stop(EV_A_ &idle_watcher);
	}
}

} /* namespace (anonymous) */

namespace js {

void
OnLoad()
{
	js_tls_hack_init();

	v8::V8::InitializeICU();

	/* Enable some cool extensions */
	V8SetFlag("--harmony");
	V8SetFlag("--harmony_typeof");
	V8SetFlag("--harmony_array_buffer");
	V8SetFlag("--harmony_typed_arrays");
	V8SetFlag("--expose_gc");

	v8::V8::SetArrayBufferAllocator(&array_buffer_allocator);

	ev_idle_init(&idle_watcher, V8Idle);
}

void
OnUnload()
{
	ev_idle_stop(&idle_watcher);
}


JS::JS()
{

}

JS::~JS()
{

}

JS *
JS::New()
{
	assert(fiber_self()->fid == 1);

	JS *js = new JS();

	js->tmplcache = mh_tmplcache_new();

	js->isolate = v8::Isolate::New();
	assert(js->isolate != NULL);

	v8::Locker locker(js->isolate);
	v8::Isolate::Scope isolate_scope(js->isolate);

	v8::HandleScope handle_scope;
	v8::Local<v8::Context> context = v8::Context::New(js->isolate);
	js->context.Reset(js->isolate, context);
	assert(!js->context.IsEmpty());

	js->isolate = v8::Isolate::GetCurrent();
	return js;
}

void
JS::Dispose()
{
	assert(fiber_self()->fid == 1);

	struct mh_tmplcache_t *tmplcache = (struct mh_tmplcache_t *)
			this->tmplcache;
	while (mh_size(tmplcache) > 0) {
		mh_int_t k = mh_first(tmplcache);

		v8::FunctionTemplate *tmpl = (v8::FunctionTemplate *)
				mh_tmplcache_node(tmplcache, k)->val;
		v8::Persistent<v8::FunctionTemplate> handle(tmpl);
		handle.Dispose();
		handle.Clear();
		mh_tmplcache_del(tmplcache, k, NULL);
	}

	if (v8::Isolate::GetCurrent() != NULL) {
		v8::Isolate::GetCurrent()->Exit();
	}

	this->isolate->Dispose();

	delete this;
}

JS *
JS::GetCurrent()
{
	return (JS*) fiber_self()->js;
}

void
JS::FiberEnsure()
{
	if (likely(GetCurrent() != NULL)) {
		assert(GetCurrent() == this);
		return;
	}

	FiberOnStart();
}


void
JS::FiberOnStart()
{
	fiber_self()->js = this;
	say_debug("js enable");

#if !defined(ENABLE_V8_HACK_LINK_TIME)
	js_tls_hack_pthread_clear();
#endif /* !defined(ENABLE_V8_HACK_LINK_TIME) */

	v8::Locker *locker = new v8::Locker(this->GetIsolate());
	fiber_self()->js_locker = locker;
	say_debug("js create locker");

	this->GetIsolate()->Enter();

	v8::ResourceConstraints constraints;
	constraints.set_stack_limit((uint32_t *) fiber_self()->coro.stack);
	v8::SetResourceConstraints(&constraints);

	v8::HandleScope handle_scope;
	this->GetPrimaryContext()->Enter();

	/*
	 * Workaround for v8 issue #1180
	 * http://code.google.com/p/v8/issues/detail?id=1180
	 */
	v8::Script::Compile(v8::String::New("void 0;"));

#if !defined(ENABLE_V8_HACK_LINK_TIME)
	js_tls_hack_pthread_to_fiber();
#endif /* !defined(ENABLE_V8_HACK_LINK_TIME) */
}

void
JS::FiberOnResume()
{
	say_debug("js fiber resume hook");
	assert (GetCurrent() == this);

#if !defined(ENABLE_V8_HACK_LINK_TIME)
	js_tls_hack_fiber_to_pthread();
#endif /* !defined(ENABLE_V8_HACK_LINK_TIME) */

	v8::Unlocker *unlocker = static_cast<v8::Unlocker *>
			(fiber_self()->js_unlocker);
	if (unlocker != NULL) {
		fiber_self()->js_unlocker = NULL;
		say_debug("js delete unlocker");
		delete unlocker;
	}
}

void
JS::FiberOnPause(void)
{
	say_debug("js fiber pause hook");
	assert (GetCurrent() == this);

#if !defined(ENABLE_V8_HACK_LINK_TIME)
	js_tls_hack_pthread_to_fiber();
#endif /* !defined(ENABLE_V8_HACK_LINK_TIME) */

	assert(fiber_self()->js_unlocker == NULL);
	say_debug("js create unlocker");
	v8::Unlocker *unlocker = new v8::Unlocker(this->GetIsolate());
	fiber_self()->js_unlocker = unlocker;

	/* Invoke V8::IdleNotificaiton in subsequents event loops */
	ev_idle_start(&idle_watcher);
}

void
JS::FiberOnStop(void)
{
	say_debug("js fiber stop hook");

	assert (GetCurrent() == this);

	this->GetIsolate()->Exit();

	v8::Unlocker *unlocker = static_cast<v8::Unlocker *>
			(fiber_self()->js_unlocker);

	if (unlocker != NULL) {
		fiber_self()->js_unlocker = NULL;
		say_debug("js delete unlocker");
		delete unlocker;
	}

	v8::Locker *locker = static_cast<v8::Locker *>(fiber_self()->js_locker);
	fiber_self()->js_locker = NULL;
	assert(locker != NULL);
	say_debug("js delete locker");
	delete locker;
}



v8::Local<v8::Object>
JS::LoadLibrary(v8::Local<v8::String> rootModule)
{
	v8::HandleScope handleScope;

	/* Init global with built-in modules */
	v8::Local<v8::Object> global = v8::Context::GetCurrent()->Global();
	js::InitGlobal(global);

	/* Eval require(TARANTOOL_JS_INIT_MODULE, false) */
	v8::Handle<v8::Object> require = global->Get(
			v8::String::NewSymbol("require")).As<v8::Object>();
	assert (!require.IsEmpty());

	v8::Handle<v8::Object> ret = js::require::call(require, rootModule,
						       false);

	return handleScope.Close(ret);
}

void
JS::TemplateCacheSet(intptr_t key, v8::Local<v8::FunctionTemplate> tmpl)
{
	struct mh_tmplcache_t *tmplcache =
			(struct mh_tmplcache_t *) this->tmplcache;

	v8::Persistent<v8::FunctionTemplate> handle(this->isolate, tmpl);
	v8::FunctionTemplate *ptr = handle.ClearAndLeak();

	const struct mh_tmplcache_node_t node = { key, ptr };
	mh_tmplcache_put(tmplcache, &node, NULL, NULL);
}

v8::Local<v8::FunctionTemplate>
JS::TemplateCacheGet(intptr_t key) const
{
	v8::HandleScope handleScope;

	struct mh_tmplcache_t *tmplcache =
			(struct mh_tmplcache_t *) this->tmplcache;

	mh_int_t k = mh_tmplcache_find(tmplcache, key, NULL);
	if (k == mh_end(tmplcache))
		return handleScope.Close(v8::Local<v8::FunctionTemplate>());

	v8::FunctionTemplate *tmpl = (v8::FunctionTemplate *)
			mh_tmplcache_node(tmplcache, k)->val;

	v8::Local<v8::FunctionTemplate> handle(tmpl);
	return handleScope.Close(handle);
}

v8::Handle<v8::Value>
InitGlobal(v8::Handle<v8::Object> global)
{
	v8::Local<v8::Function> require = js::require::constructor()->
					 GetFunction();
	global->Set(v8::String::NewSymbol("require"), require);

	/* Put built-in modules to the 'require' cache */
	js::require::cache_put(require, v8::String::NewSymbol("platform"),
			       js::platform::constructor()->GetFunction());
	js::require::cache_put(require, v8::String::NewSymbol("stub"),
			       js::stub::constructor()->GetFunction());
	js::require::cache_put(require, v8::String::NewSymbol("fiber"),
			       js::fiber::constructor()->GetFunction());
	return v8::Handle<v8::Value>();
}

v8::Handle<v8::Value>
EvalInContext(v8::Handle<v8::String> source,
		v8::Handle<v8::String> filename,
		v8::Handle<v8::Context> context)
{
	v8::HandleScope handle_scope;

	v8::Context::Scope context_scope(context);

	v8::Local<v8::Script> script = v8::Script::Compile(source, filename);
	if (script.IsEmpty()) {
		return v8::Handle<v8::Value>();
	}

	v8::Handle<v8::Value> ret = script->Run();
	if (ret.IsEmpty()) {
		return v8::Handle<v8::Value>();
	}

	return handle_scope.Close(ret);
}

v8::Handle<v8::Value>
EvalInNewContext(v8::Handle<v8::String> source,
		    v8::Handle<v8::String> filename,
		    v8::Handle<v8::Object> global)
{
	v8::HandleScope handle_scope;

	v8::Isolate *isolate = v8::Isolate::GetCurrent();
	v8::Local<v8::Context> context = v8::Context::New(isolate);

	v8::Local<v8::Object> global_proto = context->Global()->
					     GetPrototype()->ToObject();
	assert(!global_proto.IsEmpty());
	CopyObject(global_proto, global);
	InitGlobal(global_proto);

	v8::Handle<v8::Value> ret = EvalInContext(source, filename, context);
	if (ret.IsEmpty()) {
		return v8::Handle<v8::Value>();
	}

	CopyObject(global, global_proto);

	return handle_scope.Close(ret);
}

v8::Handle<v8::Value>
CopyObject(v8::Handle<v8::Object> dst, v8::Handle<v8::Object> src)
{
	v8::HandleScope handle_scope;

	v8::Local<v8::Array> props = src->GetPropertyNames();
	const uint32_t length = props->Length();
	for (uint32_t i = 0; i < length; i++) {
		v8::Local<v8::Value> key = props->Get(i);
		v8::Local<v8::Value> value = src->Get(key);
		dst->Set(key, value);
	}

	return v8::Handle<v8::Value>();
}

v8::Handle<v8::Value>
DumpObject(v8::Handle<v8::Object> src)
{
	v8::HandleScope handle_scope;

	v8::String::Utf8Value name_utf8(src->GetConstructorName());
	say_debug("Object '%.*s' dump", name_utf8.length(), *name_utf8);

	v8::Local<v8::Array> props = src->GetPropertyNames();
	const uint32_t length = props->Length();
	for (uint32_t i = 0; i < length; i++) {
		v8::Local<v8::Value> key = props->Get(i);
		v8::Local<v8::Value> value = src->Get(key);
		v8::String::Utf8Value key_utf8(key);
		v8::String::Utf8Value value_utf8(value);

		say_debug("%.*s => %.*s", key_utf8.length(), *key_utf8,
			  value_utf8.length(), *value_utf8);
	}

	return v8::Handle<v8::Value>();
}

}  /* namespace js */
