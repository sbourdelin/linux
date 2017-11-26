config DRM_I915_SPIN_REQUEST_IRQ
	int
	default 5 # microseconds
	help
	  Before sleeping waiting for a request (GPU operation) to complete,
	  we may spend some time polling for its completion. As the IRQ may
	  take a non-negligible time to setup, we do a short spin first to
	  check if the request will complete quickly.

	  May be 0 to disable the initial spin.

config DRM_I915_SPIN_REQUEST_CS
	int
	default 2 # microseconds
	help
	  After sleeping for a request (GPU operation) to complete, we will
	  be woken up on the completion of every request prior to the one
	  being waited on. For very short requests, going back to sleep and
	  be woken up again may add considerably to the wakeup latency. To
	  avoid incurring extra latency from the scheduler, we may choose to
	  spin prior to sleeping again.

	  May be 0 to disable spinning after being woken.
