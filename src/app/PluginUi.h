#pragma once

#include <gas/core/Plugins.h>

#include <vector>

class QString;
class QWidget;

// Asks for a filter's parameters (one widget per GasParam type). `values`
// comes in as the last-used values (or defaults) and is updated on OK.
bool editFilterParams(QWidget* parent, const gas::PluginFilter& filter, std::vector<double>& values);

// Lists the loaded plugins, what they registered and why others failed.
void showPluginInfo(QWidget* parent, const gas::PluginHost& host, const QString& userFolder);
