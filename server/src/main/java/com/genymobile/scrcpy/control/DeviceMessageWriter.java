package com.genymobile.scrcpy.control;

import com.genymobile.scrcpy.util.StringUtils;

import java.io.BufferedOutputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;

public class DeviceMessageWriter {

    private static final int MESSAGE_MAX_SIZE = 1 << 18; // 256k
    public static final int CLIPBOARD_TEXT_MAX_LENGTH = MESSAGE_MAX_SIZE - 5; // type: 1 byte; length: 4 bytes
    public static final int MIC_STATUS_TEXT_MAX_LENGTH = 512;

    private final DataOutputStream dos;

    public DeviceMessageWriter(OutputStream rawOutputStream) {
        dos = new DataOutputStream(new BufferedOutputStream(rawOutputStream));
    }

    public void write(DeviceMessage msg) throws IOException {
        int type = msg.getType();
        dos.writeByte(type);
        switch (type) {
            case DeviceMessage.TYPE_MIC_STATUS: {
                byte[] text = msg.getText().getBytes(StandardCharsets.UTF_8);
                int length = StringUtils.getUtf8TruncationIndex(text, MIC_STATUS_TEXT_MAX_LENGTH);
                dos.writeInt(msg.getSession());
                dos.writeByte(msg.getStatus());
                dos.writeShort(length);
                dos.write(text, 0, length);
                break;
            }
            case DeviceMessage.TYPE_MIC_PCM:
                dos.writeInt(msg.getSession());
                dos.writeLong(msg.getPts());
                dos.writeShort(msg.getData().length);
                dos.write(msg.getData());
                break;
            case DeviceMessage.TYPE_AUDIO_CLOCK:
                dos.writeInt(msg.getSession());
                dos.writeInt((int) msg.getSequence());
                dos.writeLong(msg.getPts());
                break;
            case DeviceMessage.TYPE_CLIPBOARD:
                String text = msg.getText();
                byte[] raw = text.getBytes(StandardCharsets.UTF_8);
                int len = StringUtils.getUtf8TruncationIndex(raw, CLIPBOARD_TEXT_MAX_LENGTH);
                dos.writeInt(len);
                dos.write(raw, 0, len);
                break;
            case DeviceMessage.TYPE_ACK_CLIPBOARD:
                dos.writeLong(msg.getSequence());
                break;
            case DeviceMessage.TYPE_UHID_OUTPUT:
                dos.writeShort(msg.getId());
                byte[] data = msg.getData();
                dos.writeShort(data.length);
                dos.write(data);
                break;
            default:
                throw new ControlProtocolException("Unknown event type: " + type);
        }
        dos.flush();
    }
}
