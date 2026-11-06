#pragma once

#include <QByteArray>

// Restore both the value and the distinction between unset and empty.
class ScopedEnvironment
{
      public:
	explicit ScopedEnvironment(const char *name)
	    : name(name), value(qgetenv(name)), wasSet(qEnvironmentVariableIsSet(name))
	{
	}
	~ScopedEnvironment()
	{
		if (wasSet)
			qputenv(name.constData(), value);
		else
			qunsetenv(name.constData());
	}
	ScopedEnvironment(const ScopedEnvironment &) = delete;
	ScopedEnvironment &operator=(const ScopedEnvironment &) = delete;
	void set(const QByteArray &next) { qputenv(name.constData(), next); }

      private:
	QByteArray name, value;
	bool wasSet;
};
