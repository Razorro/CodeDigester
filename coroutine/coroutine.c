#include "coroutine.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <stddef.h>
#include <string.h>
#include <stdint.h>

#if __APPLE__ && __MACH__
	#include <sys/ucontext.h>
#else 
	#include <ucontext.h>
#endif 

/*
 * 			The key to understand the implementation is, to get a fully understanding of 
 *  		getcontext, makecontext, and swapcontext.
 * 
 *  		Abstraction of these function:
 			void makecontext(ucontext_t *ucp, void (*func)(), int argc, ...);
			int swapcontext(ucontext_t *oucp, const ucontext_t *ucp);
			
			The  makecontext()  function  modifies the context pointed to by ucp (which was obtained from a
			call to getcontext(3)).  Before invoking makecontext(), the caller must allocate  a  new  stack
			for  this  context  and assign its address to ucp->uc_stack, and define a successor context and
			assign its address to ucp->uc_link.

			When this context is later activated (using setcontext(3) or swapcontext()) the  function  func
			is  called,  and passed the series of integer (int) arguments that follow argc; the caller must
			specify the number of these arguments in argc.  When this function returns, the successor  con‐
			text is activated.  If the successor context pointer is NULL, the thread exits.

			The  swapcontext()  function saves the current context in the structure pointed to by oucp, and
			then activates the context pointed to by ucp.
 * 
 * 	
 */

#define STACK_SIZE (1024*1024)
#define DEFAULT_COROUTINE 16

struct coroutine;				// forwarding-definition

struct schedule {
	char stack[STACK_SIZE];		// context size
	ucontext_t main;			// main thread execute context
	int nco;					// current corontine num
	int cap;					// current alloc max corontine num
	int running;				// the running coroutine id
	struct coroutine **co;		// the pointer to the coroutine pointer array
};

struct coroutine {
	coroutine_func func;
	void *ud;
	ucontext_t ctx;
	struct schedule * sch;
	ptrdiff_t cap;
	ptrdiff_t size;
	int status;
	char *stack;
};

struct coroutine * 
_co_new(struct schedule *S , coroutine_func func, void *ud) {
	struct coroutine * co = malloc(sizeof(*co));
	co->func = func;
	co->ud = ud;
	co->sch = S;
	co->cap = 0;
	co->size = 0;
	co->status = COROUTINE_READY;
	co->stack = NULL;
	return co;
}

void
_co_delete(struct coroutine *co) {
	free(co->stack);
	free(co);
}

struct schedule * 
coroutine_open(void) {
	struct schedule *S = malloc(sizeof(*S));
	S->nco = 0;
	S->cap = DEFAULT_COROUTINE;
	S->running = -1;
	S->co = malloc(sizeof(struct coroutine *) * S->cap);
	memset(S->co, 0, sizeof(struct coroutine *) * S->cap);
	return S;
}

void 
coroutine_close(struct schedule *S) {
	int i;
	for (i=0;i<S->cap;i++) {
		struct coroutine * co = S->co[i];
		if (co) {
			_co_delete(co);
		}
	}
	free(S->co);
	S->co = NULL;
	free(S);
}

// separate the coroutine underlying data, just return the related
// coroutine id to caller, api design principle get
int 
coroutine_new(struct schedule *S, coroutine_func func, void *ud) {
	struct coroutine *co = _co_new(S, func , ud);
	if (S->nco >= S->cap) {
		int id = S->cap;
		S->co = realloc(S->co, S->cap * 2 * sizeof(struct coroutine *));
		memset(S->co + S->cap , 0 , sizeof(struct coroutine *) * S->cap);
		S->co[S->cap] = co;
		S->cap *= 2;
		++S->nco;
		return id;
	} else {
		int i;
		for (i=0;i<S->cap;i++) {
			int id = (i+S->nco) % S->cap;
			if (S->co[id] == NULL) {
				S->co[id] = co;
				++S->nco;
				return id;
			}
		}
	}
	assert(0);
	return -1;
}

static void
mainfunc(uint32_t low32, uint32_t hi32) {
	uintptr_t ptr = (uintptr_t)low32 | ((uintptr_t)hi32 << 32);
	struct schedule *S = (struct schedule *)ptr;
	int id = S->running;
	struct coroutine *C = S->co[id];
	C->func(S,C->ud);
	_co_delete(C);
	S->co[id] = NULL;
	--S->nco;
	S->running = -1;
}

// here is just simply swap the user level execute context, 
// although we can launch several threads with individual attached scheduler,
// but when we meet some heavy computation, thread is getting blocked, those
// appended coroutine in this thread cannot get flexibly adjusted to 
// other threads, here the goroutine is a good example.
void 
coroutine_resume(struct schedule * S, int id) {
	assert(S->running == -1);
	assert(id >= 0 && id < S->cap);		
	struct coroutine *C = S->co[id];
	if (C == NULL)
		return;
	int status = C->status;
	switch(status) {
	case COROUTINE_READY:
		getcontext(&C->ctx);
		C->ctx.uc_stack.ss_sp = S->stack;		// here is the question that blocks my idea, the question is, the implementation and execution of get_context is a black-box,
		C->ctx.uc_stack.ss_size = STACK_SIZE;	// what I could get, is from observing and then make a conclusion.
		C->ctx.uc_link = &S->main;				// At least I should know the component of stack frame, to make sure
		C->status = COROUTINE_RUNNING;			// how it works.
		S->running = id;
		uintptr_t ptr = (uintptr_t)S;
		makecontext(&C->ctx, (void (*)(void)) mainfunc, 2, (uint32_t)ptr, (uint32_t)(ptr>>32));  // make the running coroutine context of mainfunc
		swapcontext(&S->main, &C->ctx);							// switch to the running coroutine context
		break;
	case COROUTINE_SUSPEND:
		memcpy(S->stack + STACK_SIZE - C->size, C->stack, C->size);
		S->running = id;
		C->status = COROUTINE_RUNNING;
		swapcontext(&S->main, &C->ctx);
		break;
	default:
		assert(0);
	}
}

static void
_save_stack(struct coroutine *C, char *top) {
	char dummy = 0;							// because of the assignment of C->ctx.uc_stack.ss_sp = S->stack, 
	assert(top - &dummy <= STACK_SIZE);		// the local variable is also associated with the stack.
	if (C->cap < top - &dummy) {			// so no need to worry about the S->stack is allocated at heap,
		free(C->stack);						// and dummy variable is allocated at stack space.
		C->cap = top-&dummy;
		C->stack = malloc(C->cap);
	}
	C->size = top - &dummy;
	memcpy(C->stack, &dummy, C->size);
}

void
coroutine_yield(struct schedule * S) {
	int id = S->running;
	assert(id >= 0);
	struct coroutine * C = S->co[id];
	assert((char *)&C > S->stack);
	_save_stack(C, S->stack + STACK_SIZE);
	C->status = COROUTINE_SUSPEND;
	S->running = -1;
	swapcontext(&C->ctx , &S->main);
}

int 
coroutine_status(struct schedule * S, int id) {
	assert(id>=0 && id < S->cap);
	if (S->co[id] == NULL) {
		return COROUTINE_DEAD;
	}
	return S->co[id]->status;
}

int 
coroutine_running(struct schedule * S) {
	return S->running;
}

