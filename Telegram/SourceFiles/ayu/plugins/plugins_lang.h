// This is the source code of AyuGram for Desktop.
//
// exteraGram-compatible plugin engine for AyuGram Desktop.
#pragma once

namespace AyuPlugins {

// UI strings of the plugin engine (Russian and English, English otherwise).
// Kept here so the feature does not depend on new keys in the cloud language packs.
[[nodiscard]] QString Tr(const char *key);
[[nodiscard]] QString Tr(const char *key, const QString &arg);

} // namespace AyuPlugins
