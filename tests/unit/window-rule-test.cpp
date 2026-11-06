#include "window-rule.hpp"

#include <QTest>

class WindowRuleTest : public QObject
{
	Q_OBJECT
      private slots:
	void focusedSelectionRequiresFullMatch()
	{
		WindowRule rule{"test.app", "Editor"};
		rule.disconnectOnTitleMismatch = false;
		WindowInfo current{"current", "test.app", "Changed title", false};
		WindowInfo focused{"focused", "test.app", "Editor", true};
		bool ambiguous;
		QCOMPARE(select_window({current, focused}, rule, "current", "current", &ambiguous), QString("current"));
		QCOMPARE(select_window({current, focused}, rule, "current", "current", &ambiguous, true),
			 QString("focused"));
		QVERIFY(!ambiguous);
		QCOMPARE(select_window({current, focused}, rule, {}, "current", &ambiguous, true), QString("focused"));
		focused.appId = "other.app";
		QCOMPARE(select_window({current, focused}, rule, "current", {}, &ambiguous, true), QString("current"));
		focused.appId = "test.app";
		focused.title = "Other";
		QCOMPARE(select_window({current, focused}, rule, "current", {}, &ambiguous, true), QString("current"));
		focused.title = "editor";
		rule.mode = TitleMatch::Exact;
		rule.caseSensitive = true;
		QCOMPARE(select_window({current, focused}, rule, "current", {}, &ambiguous, true), QString("current"));
		rule.caseSensitive = false;
		QCOMPARE(select_window({current, focused}, rule, "current", {}, &ambiguous, true), QString("focused"));
		rule.mode = TitleMatch::Regex;
		rule.title = "[";
		QCOMPARE(select_window({focused}, rule, {}, {}, &ambiguous, true), QString());
	}

	void updatedRulesUseCurrentSettings()
	{
		WindowRule rule{"old.app", "^editor$"};
		rule.mode = TitleMatch::Regex;
		QVERIFY(rule.matches({"old", "old.app", "Editor", false}));

		WindowRule settings{"new.app", "^editor$"};
		settings.mode = TitleMatch::Regex;
		settings.disconnectOnTitleMismatch = false;
		rule = rule.updated(settings);
		QVERIFY(rule.matches({"new", "new.app", "Editor", false}));
		QVERIFY(!rule.matches({"old", "old.app", "Editor", false}));
		QVERIFY(!rule.disconnectOnTitleMismatch);

		settings.caseSensitive = true;
		rule = rule.updated(settings);
		QVERIFY(!rule.matches({"new", "new.app", "Editor", false}));
		QVERIFY(rule.matches({"new", "new.app", "editor", false}));

		settings.title = "[";
		rule = rule.updated(settings);
		QVERIFY(!rule.error().isEmpty());
		QVERIFY(!rule.matches({"new", "new.app", "editor", false}));

		settings.title = "Editor*";
		settings.mode = TitleMatch::Wildcard;
		rule = rule.updated(settings);
		QVERIFY(rule.error().isEmpty());
		QVERIFY(rule.matches({"new", "new.app", "Editor - document", false}));
		QVERIFY(!rule.matches({"new", "new.app", "Edito", false}));

		settings.mode = TitleMatch::Regex;
		rule = rule.updated(settings);
		QVERIFY(rule.matches({"new", "new.app", "Edito", false}));
	}

	void titleFiltersAndStableSelection()
	{
		WindowRule rule{"test.app", "Editor"};
		WindowInfo one{"one", "test.app", "My editor", false};
		QVERIFY(rule.matches(one));
		rule.caseSensitive = true;
		QVERIFY(!rule.matches(one));
		rule.caseSensitive = false;
		rule.mode = TitleMatch::Exact;
		QVERIFY(!rule.matches(one));
		rule.title = "MY EDITOR";
		QVERIFY(rule.matches(one));
		rule.mode = TitleMatch::Wildcard;
		rule.title = "*edit?r";
		QVERIFY(rule.matches(one));
		rule.title = "*Project*";
		QVERIFY(rule.matches({"path", "test.app", "/home/mok/Project", false}));
		rule.mode = TitleMatch::Regex;
		rule.title = "^my.*editor$";
		QVERIFY(rule.matches(one));
		rule.title = "[";
		QVERIFY(!rule.error().isEmpty());
		QVERIFY(!rule.matches(one));
		rule.title = "^my.*editor$";
		QVERIFY(rule.error().isEmpty());
		QVERIFY(rule.matches(one));
		rule.caseSensitive = true;
		QVERIFY(!rule.matches(one));
		rule.caseSensitive = false;
		QVERIFY(rule.matches(one));
		rule.title.clear();
		QVERIFY(rule.matches(one));
		bool ambiguous;
		WindowInfo two{"two", "test.app", "Other", true};
		QCOMPARE(select_window({one, two}, rule, {}, {}, &ambiguous), QString());
		QVERIFY(ambiguous);
		QCOMPARE(select_window({one, two}, rule, {}, "one", &ambiguous), QString("one"));
		rule.title = "does not match";
		rule.disconnectOnTitleMismatch = false;
		QCOMPARE(select_window({one, two}, rule, "one", {}, &ambiguous), QString("one"));
		QCOMPARE(select_window({two}, rule, "one", {}, &ambiguous), QString());
		rule.disconnectOnTitleMismatch = true;
		QCOMPARE(select_window({one, two}, rule, "one", "one", &ambiguous), QString());
		rule.mode = TitleMatch::Contains;
		rule.title = "Other";
		QCOMPARE(select_window({one, two}, rule, "one", "one", &ambiguous), QString("two"));
		rule.title.clear();
		QCOMPARE(select_window({one, two}, rule, "one", {}, &ambiguous), QString("one"));
	}
};

QTEST_GUILESS_MAIN(WindowRuleTest)
#include "window-rule-test.moc"
