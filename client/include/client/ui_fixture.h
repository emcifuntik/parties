#pragma once

#include <string>

namespace Rml { class ElementDocument; }

namespace parties::client {

class AppCore;

bool IsUIFixtureScenario(const std::string& scenario);
void PopulateUIFixture(AppCore& core, const std::string& scenario, bool macos);
void ApplyUIFixtureDocument(Rml::ElementDocument* document, const std::string& scenario);

}
