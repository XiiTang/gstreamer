//! Construction-time native readiness registration. Native owns the closure
//! until its callback fences have joined, including failed construction.
use std::ffi::c_void;
type Callback = Box<dyn Fn() + Send + Sync + 'static>;
#[repr(C)]
pub(crate) struct Notify {
    notify: unsafe extern "C" fn(*mut c_void),
    data: *mut c_void,
    destroy: unsafe extern "C" fn(*mut c_void),
}
impl Notify {
    pub(crate) fn new(callback: impl Fn() + Send + Sync + 'static) -> Self {
        unsafe extern "C" fn notify(data: *mut c_void) {
            // A panic across this ABI aborts; callbacks must not panic, block
            // or re-enter native state. Native fences retain this box.
            unsafe { (&*data.cast::<Callback>())() };
        }
        unsafe extern "C" fn destroy(data: *mut c_void) {
            drop(unsafe { Box::from_raw(data.cast::<Callback>()) });
        }
        let callback: Box<Callback> = Box::new(Box::new(callback));
        Self {
            notify,
            data: Box::into_raw(callback).cast(),
            destroy,
        }
    }
}
