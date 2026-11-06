#include "pipewire-server.hpp"

#include <QFile>
#include <QTest>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

QString PipeWireServer::socketPath() const { return directory.path() + "/pipewire-m4capture-test"; }

PipeWireServer::~PipeWireServer()
{
	if (loop)
		pw_thread_loop_stop(loop);
	if (registry)
		pw_proxy_destroy(reinterpret_cast<pw_proxy *>(registry));
	if (core)
		pw_core_disconnect(core);
	if (context)
		pw_context_destroy(context);
	if (loop)
		pw_thread_loop_destroy(loop);
	if (daemon.state() != QProcess::NotRunning) {
		daemon.terminate();
		if (!daemon.waitForFinished(3000)) {
			daemon.kill();
			daemon.waitForFinished(3000);
		}
	}
}

bool PipeWireServer::start()
{
	QFile config(directory.path() + "/test.conf");
	if (!config.open(QIODevice::WriteOnly))
		return false;
	config.write(R"(
context.properties = { core.daemon = true core.name = pipewire-m4capture-test support.dbus = false }
context.spa-libs = { support.* = support/libspa-support }
context.modules = [
  { name = libpipewire-module-protocol-native }
  { name = libpipewire-module-spa-node-factory }
  { name = libpipewire-module-client-node }
  { name = libpipewire-module-link-factory }
  { name = libpipewire-module-access args = { access.force = unrestricted } }
]
context.objects = [
  { factory = spa-node-factory args = { factory.name = support.node.driver node.name = Dummy-Driver priority.driver = 20000 } }
]
)");
	config.close();
	auto environment = QProcessEnvironment::systemEnvironment();
	environment.insert("PIPEWIRE_RUNTIME_DIR", directory.path());
	// Disable user PipeWire config snippets for this isolated test daemon.
	environment.insert("PIPEWIRE_CONFIG_DIR", directory.path());
	daemon.setProcessEnvironment(environment);
	daemon.start("pipewire", {"-c", config.fileName()});
	if (!daemon.waitForStarted(3000))
		return false;
	for (int attempt = 0; attempt < 100 && !QFile::exists(socketPath()); ++attempt)
		QTest::qWait(10);
	if (!QFile::exists(socketPath())) {
		qWarning() << daemon.readAllStandardError();
		return false;
	}
	pw_init(nullptr, nullptr);
	loop = pw_thread_loop_new("m4capture-test-server", nullptr);
	if (!loop)
		return false;
	context = pw_context_new(pw_thread_loop_get_loop(loop), nullptr, 0);
	if (!context)
		return false;
	int fd = openRemote();
	if (fd < 0)
		return false;
	core = pw_context_connect_fd(context, fd, nullptr, 0);
	if (!core)
		return false;
	registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
	static const pw_registry_events events = [] {
		pw_registry_events e{};
		e.version = PW_VERSION_REGISTRY_EVENTS;
		e.global = global;
		return e;
	}();
	pw_registry_add_listener(registry, &listener, &events, this);
	return pw_thread_loop_start(loop) == 0;
}

int PipeWireServer::openRemote() const
{
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	sockaddr_un address{};
	address.sun_family = AF_UNIX;
	auto path = socketPath().toUtf8();
	if (path.size() >= int(sizeof(address.sun_path))) {
		close(fd);
		return -1;
	}
	std::memcpy(address.sun_path, path.constData(), path.size() + 1);
	if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

void PipeWireServer::global(void *data, uint32_t, uint32_t, const char *type, uint32_t, const spa_dict *props)
{
	if (!props || std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0)
		return;
	auto name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
	if (name && std::strcmp(name, "M4Capture Audio") == 0)
		++static_cast<PipeWireServer *>(data)->audioCaptures;
}
