// inFAMOUS PC Launcher — its own log: %LOCALAPPDATA%\inFAMOUS PC Launcher\launcher.log (the previous run's kept as
// launcher.prev.log). Qt's messages go there too.
#pragma once

#include <QString>

namespace Launcher::Log {

void Init();
QString Path();
QString Dir(); // %LOCALAPPDATA%\inFAMOUS PC Launcher
void Info(const QString& text);
void Warning(const QString& text);
void Error(const QString& text);

} // namespace Launcher::Log
