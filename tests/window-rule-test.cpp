#include "window-rule.hpp"

#include <QTest>

class WindowRuleTest : public QObject
{
	Q_OBJECT
      private slots:
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
