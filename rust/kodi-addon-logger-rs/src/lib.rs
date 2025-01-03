use log::{Level, LevelFilter, Log, Metadata, Record};
use std::{
    cell::UnsafeCell,
    ffi::{c_char, c_int, c_void, CString},
};

#[allow(non_camel_case_types)]
#[allow(clippy::enum_variant_names)]
#[repr(C)]
enum ADDON_LOG {
    ADDON_LOG_DEBUG = 0,
    ADDON_LOG_INFO = 1,
    ADDON_LOG_WARNING = 2,
    ADDON_LOG_ERROR = 3,
}

impl From<Level> for ADDON_LOG {
    fn from(value: Level) -> Self {
        match value {
            Level::Error => ADDON_LOG::ADDON_LOG_ERROR,
            Level::Warn => ADDON_LOG::ADDON_LOG_WARNING,
            Level::Info => ADDON_LOG::ADDON_LOG_INFO,
            Level::Debug => ADDON_LOG::ADDON_LOG_DEBUG,
            Level::Trace => ADDON_LOG::ADDON_LOG_DEBUG,
        }
    }
}

#[allow(non_camel_case_types)]
type KODI_ADDON_BACKEND_HDL = *const c_void;
type AddonLogMsgFn = unsafe extern "C" fn(KODI_ADDON_BACKEND_HDL, c_int, *const c_char);

struct Logger {
    kodi_base: KODI_ADDON_BACKEND_HDL,
    addon_log_msg_fn: Option<AddonLogMsgFn>,
}

impl Log for Logger {
    fn enabled(&self, _: &Metadata) -> bool {
        true
    }

    fn log(&self, record: &Record) {
        let mut output = format!("{}: ", record.target());

        if std::fmt::write(&mut output, *record.args()).is_err() {
            return;
        }

        let cstr = CString::new(output);

        if let Ok(cstr) = cstr {
            unsafe {
                (self.addon_log_msg_fn.unwrap())(
                    self.kodi_base,
                    ADDON_LOG::from(record.level()) as c_int,
                    cstr.as_ptr(),
                );
            }
        }
    }

    fn flush(&self) {}
}

unsafe impl Send for Logger {}
unsafe impl Sync for Logger {}

struct LoggerCell {
    inner: UnsafeCell<Logger>,
}

unsafe impl Sync for LoggerCell {}

static LOGGER: LoggerCell = LoggerCell {
    inner: UnsafeCell::new(Logger {
        kodi_base: std::ptr::null(),
        addon_log_msg_fn: None,
    }),
};

#[no_mangle]
pub extern "C" fn kodi_addon_logger_rs_initialize(
    kodi_base: KODI_ADDON_BACKEND_HDL,
    addon_log_msg_fn: AddonLogMsgFn,
) {
    let logger = unsafe { &mut *LOGGER.inner.get() };

    logger.kodi_base = kodi_base;
    logger.addon_log_msg_fn = Some(addon_log_msg_fn);

    if log::set_logger(logger).is_ok() {
        log::set_max_level(LevelFilter::Trace);
    }
}
