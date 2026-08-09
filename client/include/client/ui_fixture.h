#pragma once

#include <string>

namespace Rml { class ElementDocument; }

namespace parties::client {

class AppCore;

// Deterministic visual states used by the platform screenshot harnesses. They
// bind the production models and document without starting audio or networking.
bool IsUIFixtureScenario(const std::string& scenario);
void PopulateUIFixture(AppCore& core, const std::string& scenario, bool macos);
void ApplyUIFixtureDocument(Rml::ElementDocument* document, const std::string& scenario);

} // namespace parties::client
