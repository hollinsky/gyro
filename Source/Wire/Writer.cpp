#include "Wire/Writer.h"

#include "Wire/Connection.h"

// The one constructor that needs a `Connection` to be complete, and the whole reason this file
// exists. Keeping it out of Wire/Writer.h is what stops every generated marshalling call site from
// including Seam/EventSource.h through Wire/Connection.h — see the header.

namespace Wire
{
MessageWriter::MessageWriter(Connection& connection, ObjectId target, std::uint16_t opcode)
	: MessageWriter{ connection.Output(), target, opcode }
{}
} // namespace Wire
