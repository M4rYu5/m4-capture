#include "window-rule.hpp"

#include <QRegularExpression>

static QRegularExpression expression(const WindowRule &rule)
{
	auto pattern = rule.mode == TitleMatch::Wildcard
			   ? QRegularExpression::wildcardToRegularExpression(
				 rule.title, QRegularExpression::NonPathWildcardConversion)
			   : rule.title;
	return QRegularExpression(pattern, rule.caseSensitive ? QRegularExpression::NoPatternOption
							      : QRegularExpression::CaseInsensitiveOption);
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
	auto regex = expression(*this);
	return regex.isValid() && regex.match(window.title).hasMatch();
}

QString WindowRule::error() const
{
	if (!title.isEmpty() && mode == TitleMatch::Regex) {
		auto regex = expression(*this);
		if (!regex.isValid())
			return regex.errorString();
	}
	return {};
}

QString select_window(const QList<WindowInfo> &windows, const WindowRule &rule, const QString &current,
		      const QString &preferred, bool *ambiguous)
{
	*ambiguous = false;
	QStringList matches;
	for (const auto &window : windows) {
		if (!current.isEmpty() && window.uuid == current && window.appId == rule.appId)
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
