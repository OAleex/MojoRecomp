#pragma once

namespace mojorecomp::audio {

// Freeze/resume the host audio timeline used by the debug F6 pause. This keeps
// device playback, the XAudio render callback pump and XMA decode progression in
// lock-step with the guest debug clock instead of letting audio free-run.
void SetDebugPaused(bool paused) noexcept;

} // namespace mojorecomp::audio
