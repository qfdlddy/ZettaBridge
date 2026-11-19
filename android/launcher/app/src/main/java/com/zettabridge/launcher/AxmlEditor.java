package com.zettabridge.launcher;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * Minimal pure-Java patcher for compiled Android binary XML ({@code AndroidManifest.xml}).
 * Appends new strings to the existing string pool so existing resource-map indices remain intact,
 * then updates manifest package, version, application label, activity enable flags, and task
 * affinities.
 */
final class AxmlEditor {
    private static final int RES_STRING_POOL_TYPE = 0x0001;
    private static final int RES_XML_TYPE = 0x0003;
    private static final int RES_XML_RESOURCE_MAP_TYPE = 0x0180;
    private static final int RES_XML_START_ELEMENT_TYPE = 0x0102;

    private static final int SORTED_FLAG = 0x0001;
    private static final int UTF8_FLAG = 0x0100;

    private static final int TYPE_STRING = 0x03;
    private static final int TYPE_INT_DEC = 0x10;
    private static final int TYPE_INT_BOOLEAN = 0x12;

    private static final int ATTR_LABEL = 0x01010001;
    private static final int ATTR_NAME = 0x01010003;
    private static final int ATTR_ENABLED = 0x0101000e;
    private static final int ATTR_TASK_AFFINITY = 0x01010012;
    private static final int ATTR_VERSION_CODE = 0x0101021b;
    private static final int ATTR_VERSION_NAME = 0x0101021c;

    private AxmlEditor() {}

    static byte[] patchManifest(
            byte[] axml,
            String newPackage,
            String newLabel,
            int newVersionCode,
            String newVersionName
    ) throws IOException {
        if (axml == null || axml.length < 36) throw new IOException("AXML too short");
        ByteBuffer in = ByteBuffer.wrap(axml).order(ByteOrder.LITTLE_ENDIAN);
        int rootType = in.getShort(0) & 0xFFFF;
        int rootHeaderSize = in.getShort(2) & 0xFFFF;
        if (rootType != RES_XML_TYPE || rootHeaderSize != 8) {
            throw new IOException("Unexpected AXML root header");
        }

        int poolOffset = rootHeaderSize;
        if (poolOffset + 28 > axml.length) throw new IOException("AXML string pool header truncated");
        int poolType = in.getShort(poolOffset) & 0xFFFF;
        int poolHeaderSize = in.getShort(poolOffset + 2) & 0xFFFF;
        int poolChunkSize = in.getInt(poolOffset + 4);
        if (poolType != RES_STRING_POOL_TYPE || poolHeaderSize < 28
                || poolChunkSize < poolHeaderSize || poolChunkSize > axml.length - poolOffset) {
            throw new IOException("Invalid AXML string pool header");
        }

        int stringCount = in.getInt(poolOffset + 8);
        int styleCount = in.getInt(poolOffset + 12);
        int flags = in.getInt(poolOffset + 16);
        int stringsStart = in.getInt(poolOffset + 20);
        int stylesStart = in.getInt(poolOffset + 24);
        boolean utf8 = (flags & UTF8_FLAG) != 0;

        long offsetsEnd = (long) poolHeaderSize + ((long) stringCount + (long) styleCount) * 4L;
        if (stringCount < 0 || styleCount < 0 || offsetsEnd > poolChunkSize) {
            throw new IOException("AXML string pool table exceeds chunk bounds");
        }

        int[] origStringOffsets = new int[stringCount];
        for (int i = 0; i < stringCount; i++) {
            origStringOffsets[i] = in.getInt(poolOffset + poolHeaderSize + i * 4);
        }
        int[] origStyleOffsets = new int[styleCount];
        for (int i = 0; i < styleCount; i++) {
            origStyleOffsets[i] = in.getInt(poolOffset + poolHeaderSize + stringCount * 4 + i * 4);
        }

        if (stringsStart < offsetsEnd || stringsStart > poolChunkSize) {
            throw new IOException("Invalid AXML stringsStart");
        }
        if (styleCount > 0 && stylesStart > 0 && (stylesStart < stringsStart || stylesStart > poolChunkSize)) {
            throw new IOException("Invalid AXML stylesStart");
        }
        int stringsAbsStart = poolOffset + stringsStart;
        int stringsAbsEnd = (styleCount > 0 && stylesStart > 0)
                ? (poolOffset + stylesStart)
                : (poolOffset + poolChunkSize);
        if (stringsAbsStart > stringsAbsEnd || stringsAbsEnd > axml.length) {
            throw new IOException("Invalid AXML string blob bounds");
        }
        byte[] origStringsBlob = Arrays.copyOfRange(axml, stringsAbsStart, stringsAbsEnd);
        byte[] origStylesBlob = (styleCount > 0 && stylesStart > 0)
                ? Arrays.copyOfRange(axml, poolOffset + stylesStart, poolOffset + poolChunkSize)
                : new byte[0];

        List<String> origStrings = decodeAllStrings(origStringsBlob, origStringOffsets, utf8);

        // Append new strings without touching indices 0..stringCount-1.
        // Appending breaks alphabetical order, so clear SORTED_FLAG before writing the pool header.
        flags &= ~SORTED_FLAG;
        List<String> appended = new ArrayList<>();
        int pkgIdx = stringCount + appended.size();
        appended.add(newPackage);
        int labelIdx = stringCount + appended.size();
        appended.add(newLabel);
        int verNameIdx = -1;
        if (newVersionName != null && !newVersionName.isEmpty()) {
            verNameIdx = stringCount + appended.size();
            appended.add(newVersionName);
        }
        int switchAffIdx = stringCount + appended.size();
        appended.add(newPackage + ".switch");
        int guestAffIdx = stringCount + appended.size();
        appended.add(newPackage + ".guest");

        int totalStrings = stringCount + appended.size();
        ByteArrayOutputStream allStringsBuf = new ByteArrayOutputStream(origStringsBlob.length + 512);
        int[] newStringOffsets = new int[totalStrings];
        for (int i = 0; i < stringCount; i++) {
            newStringOffsets[i] = allStringsBuf.size();
            writeEncodedString(allStringsBuf, origStrings.get(i), utf8);
        }
        for (int i = 0; i < appended.size(); i++) {
            newStringOffsets[stringCount + i] = allStringsBuf.size();
            writeEncodedString(allStringsBuf, appended.get(i), utf8);
        }
        while ((allStringsBuf.size() & 3) != 0) {
            allStringsBuf.write(0);
        }
        byte[] newStringsBlob = allStringsBuf.toByteArray();

        int newStringsStart = poolHeaderSize + totalStrings * 4 + styleCount * 4;
        int newStylesStart = (styleCount > 0 && stylesStart > 0) ? (newStringsStart + newStringsBlob.length) : 0;
        int newPoolSize = newStringsStart + newStringsBlob.length + origStylesBlob.length;
        int poolPad = (4 - (newPoolSize & 3)) & 3;
        newPoolSize += poolPad;

        byte[] restXml = Arrays.copyOfRange(axml, poolOffset + poolChunkSize, axml.length);
        patchXmlBody(restXml, origStrings, pkgIdx, labelIdx, newVersionCode, verNameIdx, switchAffIdx, guestAffIdx);

        ByteArrayOutputStream out = new ByteArrayOutputStream(rootHeaderSize + newPoolSize + restXml.length);
        byte[] rootHdr = new byte[8];
        ByteBuffer.wrap(rootHdr).order(ByteOrder.LITTLE_ENDIAN)
                .putShort((short) RES_XML_TYPE)
                .putShort((short) 8)
                .putInt(8 + newPoolSize + restXml.length);
        out.write(rootHdr);

        byte[] poolHdr = new byte[poolHeaderSize];
        ByteBuffer ph = ByteBuffer.wrap(poolHdr).order(ByteOrder.LITTLE_ENDIAN);
        ph.putShort((short) RES_STRING_POOL_TYPE);
        ph.putShort((short) poolHeaderSize);
        ph.putInt(newPoolSize);
        ph.putInt(totalStrings);
        ph.putInt(styleCount);
        ph.putInt(flags);
        ph.putInt(newStringsStart);
        ph.putInt(newStylesStart);
        out.write(poolHdr);

        byte[] offsetsBytes = new byte[(totalStrings + styleCount) * 4];
        ByteBuffer ob = ByteBuffer.wrap(offsetsBytes).order(ByteOrder.LITTLE_ENDIAN);
        for (int off : newStringOffsets) ob.putInt(off);
        for (int off : origStyleOffsets) ob.putInt(off);
        out.write(offsetsBytes);

        out.write(newStringsBlob);
        out.write(origStylesBlob);
        for (int i = 0; i < poolPad; i++) out.write(0);
        out.write(restXml);
        return out.toByteArray();
    }

    private static void patchXmlBody(
            byte[] xml,
            List<String> origStrings,
            int pkgIdx,
            int labelIdx,
            int newVersionCode,
            int verNameIdx,
            int switchAffIdx,
            int guestAffIdx
    ) {
        ByteBuffer buf = ByteBuffer.wrap(xml).order(ByteOrder.LITTLE_ENDIAN);
        int[] resourceMap = new int[0];
        int pos = 0;
        while (pos + 8 <= xml.length) {
            int type = buf.getShort(pos) & 0xFFFF;
            int headerSize = buf.getShort(pos + 2) & 0xFFFF;
            int chunkSize = buf.getInt(pos + 4);
            if (chunkSize < 8 || headerSize < 8 || headerSize > chunkSize || chunkSize > xml.length - pos) break;

            if (type == RES_XML_RESOURCE_MAP_TYPE) {
                int count = (chunkSize - headerSize) / 4;
                resourceMap = new int[count];
                for (int i = 0; i < count; i++) {
                    resourceMap[i] = buf.getInt(pos + headerSize + i * 4);
                }
            } else if (type == RES_XML_START_ELEMENT_TYPE && headerSize >= 16 && headerSize + 20 <= chunkSize) {
                int extPos = pos + headerSize;
                int elemNameIdx = buf.getInt(extPos + 4);
                String elemName = getString(origStrings, elemNameIdx);
                int attrStart = buf.getShort(extPos + 8) & 0xFFFF;
                int attrSize = buf.getShort(extPos + 10) & 0xFFFF;
                int attrCount = buf.getShort(extPos + 12) & 0xFFFF;
                int attrsBase = extPos + attrStart;
                int chunkEnd = pos + chunkSize;
                if (attrSize < 20 || attrsBase < extPos || attrsBase > chunkEnd) {
                    pos += chunkSize;
                    continue;
                }

                String activityName = null;
                if ("activity".equals(elemName)) {
                    for (int i = 0; i < attrCount; i++) {
                        long attrPos = (long) attrsBase + (long) i * (long) attrSize;
                        if (attrPos + 20 > chunkEnd) break;
                        int a = (int) attrPos;
                        int nameIdx = buf.getInt(a + 4);
                        int resId = resIdOf(resourceMap, nameIdx);
                        if (resId == ATTR_NAME) {
                            activityName = getString(origStrings, buf.getInt(a + 16));
                            break;
                        }
                    }
                }

                for (int i = 0; i < attrCount; i++) {
                    long attrPos = (long) attrsBase + (long) i * (long) attrSize;
                    if (attrPos + 20 > chunkEnd) break;
                    int a = (int) attrPos;
                    int nsIdx = buf.getInt(a);
                    int nameIdx = buf.getInt(a + 4);
                    String attrName = getString(origStrings, nameIdx);
                    int resId = resIdOf(resourceMap, nameIdx);

                    if ("manifest".equals(elemName)) {
                        if (nsIdx == -1 && "package".equals(attrName)) {
                            writeStringAttr(buf, a, pkgIdx);
                        } else if (resId == ATTR_VERSION_CODE && newVersionCode > 0) {
                            buf.putInt(a + 8, -1);
                            buf.put(a + 15, (byte) TYPE_INT_DEC);
                            buf.putInt(a + 16, newVersionCode);
                        } else if (resId == ATTR_VERSION_NAME && verNameIdx >= 0) {
                            writeStringAttr(buf, a, verNameIdx);
                        }
                    } else if ("application".equals(elemName)) {
                        if (resId == ATTR_LABEL) {
                            writeStringAttr(buf, a, labelIdx);
                        }
                    } else if ("activity".equals(elemName)) {
                        if (resId == ATTR_ENABLED && activityName != null) {
                            if (activityName.endsWith(".LibraryActivity")) {
                                writeBoolAttr(buf, a, false);
                            } else if (activityName.endsWith(".StandaloneLaunchActivity")) {
                                writeBoolAttr(buf, a, true);
                            }
                        } else if (resId == ATTR_TASK_AFFINITY) {
                            String cur = getString(origStrings, buf.getInt(a + 16));
                            if (cur != null && cur.endsWith(".switch")) {
                                writeStringAttr(buf, a, switchAffIdx);
                            } else if (cur != null && cur.endsWith(".guest")) {
                                writeStringAttr(buf, a, guestAffIdx);
                            }
                        }
                    }
                }
            }
            pos += chunkSize;
        }
    }

    private static void writeStringAttr(ByteBuffer buf, int attrOffset, int strIdx) {
        buf.putInt(attrOffset + 8, strIdx);
        buf.put(attrOffset + 15, (byte) TYPE_STRING);
        buf.putInt(attrOffset + 16, strIdx);
    }

    private static void writeBoolAttr(ByteBuffer buf, int attrOffset, boolean value) {
        buf.putInt(attrOffset + 8, -1);
        buf.put(attrOffset + 15, (byte) TYPE_INT_BOOLEAN);
        buf.putInt(attrOffset + 16, value ? -1 : 0);
    }

    private static int resIdOf(int[] map, int idx) {
        return (idx >= 0 && idx < map.length) ? map[idx] : 0;
    }

    private static String getString(List<String> pool, int idx) {
        return (idx >= 0 && idx < pool.size()) ? pool.get(idx) : null;
    }

    private static List<String> decodeAllStrings(byte[] blob, int[] offsets, boolean utf8) {
        List<String> out = new ArrayList<>(offsets.length);
        ByteBuffer bb = utf8 ? null : ByteBuffer.wrap(blob).order(ByteOrder.LITTLE_ENDIAN);
        for (int off : offsets) {
            if (off < 0 || off >= blob.length) {
                out.add("");
                continue;
            }
            if (utf8) {
                int p = off;
                // Skip UTF-16 char length (1 or 2 bytes)
                if ((blob[p] & 0x80) != 0) p += 2;
                else p += 1;
                if (p >= blob.length) {
                    out.add("");
                    continue;
                }
                int byteLen;
                if ((blob[p] & 0x80) != 0) {
                    if (p + 1 >= blob.length) {
                        out.add("");
                        continue;
                    }
                    byteLen = ((blob[p] & 0x7F) << 8) | (blob[p + 1] & 0xFF);
                    p += 2;
                } else {
                    byteLen = blob[p] & 0x7F;
                    p += 1;
                }
                int safeLen = Math.min(byteLen, Math.max(0, blob.length - p));
                out.add(decodeMutf8(blob, p, safeLen));
            } else {
                int p = off;
                if (p + 2 > blob.length) {
                    out.add("");
                    continue;
                }
                int charLen = bb.getShort(p) & 0xFFFF;
                p += 2;
                if ((charLen & 0x8000) != 0) {
                    if (p + 2 > blob.length) {
                        out.add("");
                        continue;
                    }
                    charLen = ((charLen & 0x7FFF) << 16) | (bb.getShort(p) & 0xFFFF);
                    p += 2;
                }
                long byteLen = (long) charLen * 2L;
                int safeBytes = (int) Math.min(byteLen, Math.max(0, blob.length - p));
                out.add(new String(blob, p, safeBytes, StandardCharsets.UTF_16LE));
            }
        }
        return out;
    }

    private static String decodeMutf8(byte[] data, int offset, int length) {
        // Android UTF-8 string pools use Modified UTF-8 (CESU-8 for supplementary characters).
        StringBuilder sb = new StringBuilder(length);
        int i = offset;
        int end = offset + length;
        while (i < end) {
            int b0 = data[i++] & 0xFF;
            if (b0 < 0x80) {
                sb.append((char) b0);
            } else if ((b0 & 0xE0) == 0xC0 && i < end) {
                int b1 = data[i++] & 0xFF;
                sb.append((char) (((b0 & 0x1F) << 6) | (b1 & 0x3F)));
            } else if ((b0 & 0xF0) == 0xE0 && i + 1 < end) {
                int b1 = data[i++] & 0xFF;
                int b2 = data[i++] & 0xFF;
                sb.append((char) (((b0 & 0x0F) << 12) | ((b1 & 0x3F) << 6) | (b2 & 0x3F)));
            } else if ((b0 & 0xF8) == 0xF0 && i + 2 < end) {
                int b1 = data[i++] & 0xFF;
                int b2 = data[i++] & 0xFF;
                int b3 = data[i++] & 0xFF;
                int cp = ((b0 & 0x07) << 18) | ((b1 & 0x3F) << 12) | ((b2 & 0x3F) << 6) | (b3 & 0x3F);
                sb.appendCodePoint(cp);
            } else {
                sb.append('\uFFFD');
            }
        }
        return sb.toString();
    }

    private static byte[] encodeMutf8(String s) {
        ByteArrayOutputStream buf = new ByteArrayOutputStream(s.length());
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c >= 0x0001 && c <= 0x007F) {
                buf.write(c);
            } else if (c <= 0x07FF) {
                buf.write(0xC0 | ((c >> 6) & 0x1F));
                buf.write(0x80 | (c & 0x3F));
            } else {
                buf.write(0xE0 | ((c >> 12) & 0x0F));
                buf.write(0x80 | ((c >> 6) & 0x3F));
                buf.write(0x80 | (c & 0x3F));
            }
        }
        return buf.toByteArray();
    }

    private static void writeEncodedString(ByteArrayOutputStream out, String s, boolean utf8) throws IOException {
        if (utf8) {
            byte[] bytes = encodeMutf8(s);
            writeUtf8Length(out, s.length());
            writeUtf8Length(out, bytes.length);
            out.write(bytes, 0, bytes.length);
            out.write(0);
        } else {
            byte[] bytes = s.getBytes(StandardCharsets.UTF_16LE);
            int chars = s.length();
            if (chars >= 0x8000) {
                int hi = 0x8000 | (chars >> 16);
                int lo = chars & 0xFFFF;
                out.write(hi & 0xFF);
                out.write((hi >> 8) & 0xFF);
                out.write(lo & 0xFF);
                out.write((lo >> 8) & 0xFF);
            } else {
                out.write(chars & 0xFF);
                out.write((chars >> 8) & 0xFF);
            }
            out.write(bytes, 0, bytes.length);
            out.write(0);
            out.write(0);
        }
    }

    private static void writeUtf8Length(ByteArrayOutputStream out, int len) throws IOException {
        if (len < 0 || len >= 0x8000) {
            throw new IOException("AXML UTF-8 string length out of range: " + len);
        }
        if (len >= 0x80) {
            out.write(0x80 | ((len >> 8) & 0x7F));
            out.write(len & 0xFF);
        } else {
            out.write(len & 0x7F);
        }
    }
}
