// iOS stub: no UI sound effects in phase 1.
// AudioEngine already null-checks sound_player_ before calling mix_output.
#include <client/sound_player.h>

namespace parties::client {

SoundPlayer::SoundPlayer() = default;

void SoundPlayer::play(Effect) {}

void SoundPlayer::mix_output(float*, int) {}

} // namespace parties::client
