#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace intelvst::bridge
{
    static constexpr juce::uint32 magic = 0x49565752; // IVWR

    enum class MessageType : juce::uint32
    {
        hello = 1,
        loadPlugin = 2,
        prepare = 3,
        process = 4,
        processResult = 5,
        shutdown = 6,
        error = 7,
        openEditor = 8
    };

    struct Header
    {
        juce::uint32 protocolMagic = magic;
        juce::uint32 type = 0;
        juce::uint32 payloadBytes = 0;
    };

    inline bool writeExact (juce::StreamingSocket& socket, const void* data, int bytes)
    {
        const auto* current = static_cast<const char*> (data);
        auto remaining = bytes;

        while (remaining > 0)
        {
            const auto written = socket.write (current, remaining);
            if (written <= 0)
                return false;

            current += written;
            remaining -= written;
        }

        return true;
    }

    inline bool readExact (juce::StreamingSocket& socket, void* data, int bytes)
    {
        auto* current = static_cast<char*> (data);
        auto remaining = bytes;

        while (remaining > 0)
        {
            const auto received = socket.read (current, remaining, true);
            if (received <= 0)
                return false;

            current += received;
            remaining -= received;
        }

        return true;
    }

    inline bool sendMessage (juce::StreamingSocket& socket,
                             MessageType type,
                             const juce::MemoryBlock& payload = {})
    {
        const Header header { magic,
                              static_cast<juce::uint32> (type),
                              static_cast<juce::uint32> (payload.getSize()) };

        return writeExact (socket, &header, sizeof (header))
            && (payload.isEmpty() || writeExact (socket, payload.getData(), static_cast<int> (payload.getSize())));
    }

    inline bool receiveMessage (juce::StreamingSocket& socket,
                                MessageType& type,
                                juce::MemoryBlock& payload)
    {
        Header header;
        if (! readExact (socket, &header, sizeof (header)))
            return false;

        if (header.protocolMagic != magic)
            return false;

        payload.setSize (header.payloadBytes, false);

        if (header.payloadBytes > 0
            && ! readExact (socket, payload.getData(), static_cast<int> (header.payloadBytes)))
            return false;

        type = static_cast<MessageType> (header.type);
        return true;
    }

    inline void writeString (juce::MemoryOutputStream& out, const juce::String& text)
    {
        const auto utf8 = text.toRawUTF8();
        const auto bytes = static_cast<juce::uint32> (std::strlen (utf8));
        out.writeInt (static_cast<int> (bytes));
        out.write (utf8, bytes);
    }

    inline juce::String readString (juce::MemoryInputStream& in)
    {
        const auto bytes = in.readInt();
        juce::MemoryBlock block (static_cast<size_t> (juce::jmax (0, bytes)));
        in.read (block.getData(), bytes);
        return juce::String::fromUTF8 (static_cast<const char*> (block.getData()), bytes);
    }
}
