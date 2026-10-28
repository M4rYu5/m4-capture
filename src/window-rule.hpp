#pragma once

#include <QList>
#include <QString>

struct WindowInfo {
	QString uuid;
	QString appId;
	QString title;
	bool active = false;
};

enum class TitleMatch { Contains, Exact, Wildcard, Regex };

struct WindowRule {
	QString appId;
	QString title;
	TitleMatch mode = TitleMatch::Contains;
	bool caseSensitive = false;
	bool disconnectOnTitleMismatch = true;
	bool matches(const WindowInfo &window) const;
	QString error() const;
};

// Keep a live selection through title changes unless continuous title matching is enabled.
QString select_window(const QList<WindowInfo> &windows, const WindowRule &rule, const QString &current,
		      const QString &preferred, bool *ambiguous);
