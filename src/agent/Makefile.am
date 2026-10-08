AM_CPPFLAGS = -I$(top_srcdir)/include

noinst_LIBRARIES = libagent.a

libagent_a_SOURCES = \
	protocol/agent_protocol.cpp \
	protocol/sha256.cpp \
	replay/native_archive.cpp \
	bridge/agent_bridge.cpp \
	debugger/debugger_adapter.cpp \
	parser/debugger_output_parser.cpp \
	replay/replay_timeline.cpp \
	replay/exec_observer.cpp \
	replay/state_capture.cpp \
	video/video_snapshot.cpp \
	video/raster_stream.cpp \
	checkpoint/checkpoint.cpp \
	server/agent_server.cpp \
	trace/trace_store.cpp \
	transport/local_transport.cpp \
	transport/rpc_transport.h
