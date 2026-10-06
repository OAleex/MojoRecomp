use discord_rich_presence::{DiscordIpc, DiscordIpcClient, activity};
use std::sync::{Mutex, mpsc};
use std::thread;
use std::time::Duration;

const DISCORD_ACTIVITY_NAME: &str = "MojoRecomp";
const RETRY_DELAY: Duration = Duration::from_secs(5);

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum PresenceActivity {
    Launcher,
    CrashOfTheTitans,
}

impl PresenceActivity {
    fn details(self) -> &'static str {
        match self {
            Self::Launcher => "In the launcher",
            Self::CrashOfTheTitans => "Crash of the Titans",
        }
    }
}

pub fn valid_application_id(value: &str) -> Option<String> {
    let value = value.trim();
    if value.is_empty()
        || value.len() > 20
        || !value.bytes().all(|byte| byte.is_ascii_digit())
        || value.bytes().all(|byte| byte == b'0')
    {
        return None;
    }
    Some(value.to_string())
}

trait PresenceBackend: Send {
    fn publish(&mut self, activity: PresenceActivity) -> bool;
    fn clear(&mut self);
}

struct DiscordRpcBackend {
    application_id: Option<String>,
    client: Option<DiscordIpcClient>,
}

impl DiscordRpcBackend {
    fn new(application_id: Option<String>) -> Self {
        Self {
            application_id,
            client: None,
        }
    }

    fn connect(&mut self) -> bool {
        if self.client.is_some() {
            return true;
        }
        let Some(application_id) = self.application_id.as_deref() else {
            return true;
        };
        let mut client = DiscordIpcClient::new(application_id);
        if client.connect().is_err() {
            return false;
        }
        self.client = Some(client);
        true
    }

    fn disconnect(&mut self) {
        if let Some(mut client) = self.client.take() {
            let _ = client.close();
        }
    }
}

impl PresenceBackend for DiscordRpcBackend {
    fn publish(&mut self, current: PresenceActivity) -> bool {
        if self.application_id.is_none() {
            return true;
        }
        if !self.connect() {
            return false;
        }

        let payload = activity::Activity::new()
            .name(DISCORD_ACTIVITY_NAME)
            .details(current.details())
            .activity_type(activity::ActivityType::Playing)
            .status_display_type(activity::StatusDisplayType::Name);
        if self
            .client
            .as_mut()
            .is_some_and(|client| client.set_activity(payload.clone()).is_ok())
        {
            return true;
        }

        self.disconnect();
        if !self.connect() {
            return false;
        }
        let applied = self
            .client
            .as_mut()
            .is_some_and(|client| client.set_activity(payload).is_ok());
        if !applied {
            self.disconnect();
        }
        applied
    }

    fn clear(&mut self) {
        if self.application_id.is_none() {
            return;
        }
        if self.connect()
            && self
                .client
                .as_mut()
                .is_some_and(|client| client.clear_activity().is_ok())
        {
            return;
        }
        self.disconnect();
    }
}

enum WorkerCommand {
    Publish(PresenceActivity),
    Clear,
    Shutdown(mpsc::Sender<()>),
}

fn spawn_worker<B: PresenceBackend + 'static>(mut backend: B) -> mpsc::Sender<WorkerCommand> {
    let (sender, receiver) = mpsc::channel();
    thread::spawn(move || {
        let mut desired = None;
        let mut applied = None;

        loop {
            let timeout = if desired.is_some() && desired != applied {
                RETRY_DELAY
            } else {
                Duration::from_secs(60)
            };
            match receiver.recv_timeout(timeout) {
                Ok(WorkerCommand::Publish(next)) => {
                    desired = Some(next);
                    if applied != desired && backend.publish(next) {
                        applied = desired;
                    }
                }
                Ok(WorkerCommand::Clear) => {
                    desired = None;
                    applied = None;
                    backend.clear();
                }
                Ok(WorkerCommand::Shutdown(done)) => {
                    backend.clear();
                    let _ = done.send(());
                    return;
                }
                Err(mpsc::RecvTimeoutError::Timeout) => {
                    if let Some(next) = desired
                        && applied != desired
                        && backend.publish(next)
                    {
                        applied = desired;
                    }
                }
                Err(mpsc::RecvTimeoutError::Disconnected) => {
                    backend.clear();
                    return;
                }
            }
        }
    });
    sender
}

#[derive(Debug)]
struct State {
    enabled: bool,
    current: PresenceActivity,
    cot_pid: Option<u32>,
}

pub struct PresenceController {
    state: Mutex<State>,
    worker: mpsc::Sender<WorkerCommand>,
}

impl PresenceController {
    pub fn new(enabled: bool, application_id: Option<String>) -> Self {
        let controller = Self {
            state: Mutex::new(State {
                enabled,
                current: PresenceActivity::Launcher,
                cot_pid: None,
            }),
            worker: spawn_worker(DiscordRpcBackend::new(application_id)),
        };
        if enabled {
            let _ = controller
                .worker
                .send(WorkerCommand::Publish(PresenceActivity::Launcher));
        }
        controller
    }

    pub fn set_enabled(&self, enabled: bool) {
        let current = self.state.lock().ok().map(|mut state| {
            state.enabled = enabled;
            state.current
        });
        if enabled {
            if let Some(current) = current {
                let _ = self.worker.send(WorkerCommand::Publish(current));
            }
        } else {
            let _ = self.worker.send(WorkerCommand::Clear);
        }
    }

    pub fn cot_started(&self, pid: u32) {
        if let Ok(mut state) = self.state.lock() {
            state.cot_pid = Some(pid);
            state.current = PresenceActivity::CrashOfTheTitans;
            if state.enabled {
                let _ = self.worker.send(WorkerCommand::Publish(state.current));
            }
        }
    }

    pub fn cot_stopped(&self, pid: u32) {
        if let Ok(mut state) = self.state.lock() {
            if state.cot_pid != Some(pid) {
                return;
            }
            state.cot_pid = None;
            state.current = PresenceActivity::Launcher;
            if state.enabled {
                let _ = self.worker.send(WorkerCommand::Publish(state.current));
            }
        }
    }

    pub fn shutdown(&self) {
        let (done_tx, done_rx) = mpsc::channel();
        if self.worker.send(WorkerCommand::Shutdown(done_tx)).is_ok() {
            let _ = done_rx.recv_timeout(Duration::from_secs(1));
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::{
        Arc,
        atomic::{AtomicUsize, Ordering},
    };

    struct CountingBackend {
        publishes: Arc<AtomicUsize>,
        clears: Arc<AtomicUsize>,
    }

    impl PresenceBackend for CountingBackend {
        fn publish(&mut self, _: PresenceActivity) -> bool {
            self.publishes.fetch_add(1, Ordering::Relaxed);
            true
        }

        fn clear(&mut self) {
            self.clears.fetch_add(1, Ordering::Relaxed);
        }
    }

    #[test]
    fn activity_text_is_process_only() {
        assert_eq!(PresenceActivity::Launcher.details(), "In the launcher");
        assert_eq!(
            PresenceActivity::CrashOfTheTitans.details(),
            "Crash of the Titans"
        );
    }

    #[test]
    fn invalid_application_ids_are_rejected() {
        assert_eq!(valid_application_id(""), None);
        assert_eq!(valid_application_id("abc"), None);
        assert_eq!(valid_application_id("0000"), None);
        assert_eq!(valid_application_id("123456789012345678901"), None);
        assert_eq!(
            valid_application_id(" 123456789012345678 "),
            Some("123456789012345678".into())
        );
    }

    #[test]
    fn worker_deduplicates_equal_activity() {
        let publishes = Arc::new(AtomicUsize::new(0));
        let clears = Arc::new(AtomicUsize::new(0));
        let sender = spawn_worker(CountingBackend {
            publishes: publishes.clone(),
            clears: clears.clone(),
        });
        sender
            .send(WorkerCommand::Publish(PresenceActivity::Launcher))
            .unwrap();
        sender
            .send(WorkerCommand::Publish(PresenceActivity::Launcher))
            .unwrap();
        let (done_tx, done_rx) = mpsc::channel();
        sender.send(WorkerCommand::Shutdown(done_tx)).unwrap();
        done_rx.recv_timeout(Duration::from_secs(1)).unwrap();
        assert_eq!(publishes.load(Ordering::Relaxed), 1);
        assert_eq!(clears.load(Ordering::Relaxed), 1);
    }
}
