#include "window-rule.hpp"

#include <utility>

WindowRule::WindowRule(QString appId, QString title) : appId(std::move(appId)), title(std::move(title)) {}

WindowRule WindowRule::updated(const WindowRule &settings) const
{
	auto rule = *this;
	rule.appId = settings.appId;
	rule.title = settings.title;
	rule.mode = settings.mode;
	rule.caseSensitive = settings.caseSensitive;
	rule.disconnectOnTitleMismatch = settings.disconnectOnTitleMismatch;
	return rule;
}

const QRegularExpression &WindowRule::expression() const
{
	if (compiledTitle == title && compiledMode == mode && compiledCaseSensitive == caseSensitive)
		return compiledExpression;
	compiledTitle = title;
	compiledMode = mode;
	compiledCaseSensitive = caseSensitive;
	auto pattern =
	    mode == TitleMatch::Wildcard
		? QRegularExpression::wildcardToRegularExpression(title, QRegularExpression::NonPathWildcardConversion)
		: title;
	compiledExpression = QRegularExpression(pattern, caseSensitive ? QRegularExpression::NoPatternOption
								       : QRegularExpression::CaseInsensitiveOption);
	return compiledExpression;
}

bool WindowRule::matches(const WindowInfo &window) const
{
	if (appId.isEmpty() || appId != window.appId)
		return false;
	if (title.isEmpty())
		return true;
	auto sensitivity = caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive;
	if (mode == TitleMatch::Exact)
		return window.title.compare(title, sensitivity) == 0;
	if (mode == TitleMatch::Contains)
		return window.title.contains(title, sensitivity);
	const auto &regex = expression();
	return regex.isValid() && regex.match(window.title).hasMatch();
}

QString WindowRule::error() const
{
	if (!title.isEmpty() && mode == TitleMatch::Regex) {
		const auto &regex = expression();
		if (!regex.isValid())
			return regex.errorString();
	}
	return {};
}

QString select_window(const QList<WindowInfo> &windows, const WindowRule &rule, const QString &current,
		      const QString &preferred, bool *ambiguous, bool followFocus)
{
	*ambiguous = false;
	if (followFocus)
		for (const auto &window : windows)
			if (window.active && rule.matches(window))
				return window.uuid;
	QStringList matches;
	for (const auto &window : windows) {
		if (!current.isEmpty() && window.uuid == current && window.appId == rule.appId &&
		    (!rule.disconnectOnTitleMismatch || rule.matches(window)))
			return current;
		if (rule.matches(window))
			matches.append(window.uuid);
	}
	if (!preferred.isEmpty() && matches.contains(preferred))
		return preferred;
	if (matches.size() == 1)
		return matches.front();
	*ambiguous = matches.size() > 1;
	return {};
}
