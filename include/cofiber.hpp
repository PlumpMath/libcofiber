
#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include <utility>
#include <new>
#include <vector>

template<typename T,
		typename = decltype(std::declval<T>().await_ready())>
T &&cofiber_awaiter(T &&awaiter) {
	return std::forward<T>(awaiter);
}

namespace _cofiber_private {
	extern "C" void _cofiber_enter(void *argument, void (*function) (void *, void *),
			void *initial_sp);
	extern "C" void _cofiber_restore(void *argument, void (*hook) (void *, void *),
			void *restore_sp);

	template<typename F>
	void enter(F functor, void *initial_sp) {
		_cofiber_enter(&functor, [] (void *argument, void *original_sp) {
			F stolen = std::move(*static_cast<F *>(argument));
			stolen(original_sp);
		}, initial_sp);
	}

	template<typename F>
	void restore(F functor, void *initial_sp) {
		_cofiber_restore(&functor, [] (void *argument, void *caller_sp) {
			F stolen = std::move(*static_cast<F *>(argument));
			stolen(caller_sp);
		}, initial_sp);
	}

	struct state_struct {
		state_struct()
		: suspended_sp(nullptr) { }

		void *suspended_sp;
	};

	struct activation_struct {
		activation_struct(state_struct *state, void *caller_sp)
		: state(state), caller_sp(caller_sp) { }

		state_struct *state;
		void *caller_sp;
	};

	struct destroy_exception { };

	thread_local std::vector<activation_struct> stack;
} // namespace _cofiber_private

namespace cofiber {
	template<typename P = void>
	struct coroutine_handle {
		coroutine_handle from_address(void *address) {
			auto state = static_cast<_cofiber_private::state_struct *>(address);
			return coroutine_handle(state);
		}

		coroutine_handle()
		: _state(nullptr) { }

		coroutine_handle(_cofiber_private::state_struct *state)
		: _state(state) { }

		void *address() {
			return _state;
		}

		explicit operator bool () {
			return _state != nullptr;
		}

		void resume() {
			_cofiber_private::restore([&] (void *caller_sp) {
				_cofiber_private::stack.push_back({ _state, caller_sp });
			}, _state->suspended_sp);
		}

		void destroy() {
			assert(!"Destory was called");
		}

	private:
		_cofiber_private::state_struct *_state;
	};

	template<typename T>
	struct coroutine_traits {
		using promise_type = typename T::promise_type;
	};

	struct suspend_never {
		bool await_ready() { return true; }
		void await_suspend(coroutine_handle<>) { }
		void await_resume() { }
	};

	struct suspend_always {
		bool await_ready() { return false; }
		void await_suspend(coroutine_handle<>) { }
		void await_resume() { }
	};

	struct no_future {
		struct promise_type {
			no_future get_return_object(coroutine_handle<>) {
				return no_future();
			}

			auto initial_suspend() { return suspend_never(); }
			auto final_suspend() { return suspend_never(); }
		};
	};
} // namespace cofiber

template<typename T>
decltype(cofiber_awaiter(std::declval<T &&>()).await_resume()) cofiber_await(T &&expression) {
	auto awaiter = cofiber_awaiter(std::forward<T>(expression));

	if(!awaiter.await_ready()) {
		_cofiber_private::restore([&] (void *coroutine_sp) {
			auto state = _cofiber_private::stack.back().state;
			_cofiber_private::stack.pop_back();

			state->suspended_sp = coroutine_sp;

			awaiter.await_suspend(cofiber::coroutine_handle<>(state));
		}, _cofiber_private::stack.back().caller_sp);
	}

	return awaiter.await_resume();
}

template<typename X, typename F>
X cofiber_routine(F functor) {
	using P = typename cofiber::coroutine_traits<X>::promise_type;

	size_t stack_size = 0x100000;
	char *sp = (char *)(operator new(stack_size)) + stack_size;
	
	// allocate both the coroutine state and the promise on the fiber stack
	sp -= sizeof(_cofiber_private::state_struct);
	assert(uintptr_t(sp) % alignof(_cofiber_private::state_struct) == 0);
	auto state = new (sp) _cofiber_private::state_struct;

	sp -= sizeof(P);
	assert(uintptr_t(sp) % alignof(P) == 0);
	auto promise = new (sp) P;

	_cofiber_private::enter([=] (void *original_sp) {
		_cofiber_private::stack.push_back({ state, original_sp });

		try {
			cofiber_await(promise->initial_suspend());
			functor();
			cofiber_await(promise->final_suspend());
		}catch(_cofiber_private::destroy_exception &) {
			// ignore the exception that is thrown by destroy()
		}catch(...) {
			std::terminate();
		}

		_cofiber_private::restore([&] (void *coroutine_sp) {
			auto state = _cofiber_private::stack.back().state;
			_cofiber_private::stack.pop_back();

		}, _cofiber_private::stack.back().caller_sp);
	}, sp);

	return promise->get_return_object(cofiber::coroutine_handle<>(state));
}
