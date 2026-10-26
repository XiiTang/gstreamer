#[derive(Default)]
pub struct Recording(std::sync::Mutex<Vec<(String, i32)>>);
impl boundless_media::rtp::DiagnosticSink for Recording {
    fn offer(&self, fact: boundless_media::Fact, _message: &[u8]) {
        self.0
            .lock()
            .unwrap()
            .push((fact.domain.to_string_lossy().into_owned(), fact.code));
    }
}
impl Recording {
    pub fn assert_none(&self) {
        assert!(self.0.lock().unwrap().is_empty());
    }
    pub fn first(&self) -> Option<(String, i32)> {
        self.0.lock().unwrap().first().cloned()
    }
    pub fn count(&self) -> usize {
        self.0.lock().unwrap().len()
    }
}
