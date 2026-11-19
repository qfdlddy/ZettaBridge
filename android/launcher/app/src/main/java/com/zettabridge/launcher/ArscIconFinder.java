package com.zettabridge.launcher;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * Parses {@code resources.arsc} to discover the exact ZIP entry paths (even after AGP resource
 * path shortening) and densities for {@code R.mipmap.ic_launcher} and
 * {@code R.mipmap.ic_launcher_round}.
 */
final class ArscIconFinder {
    private static final int RES_STRING_POOL_TYPE = 0x0001;
    private static final int RES_TABLE_TYPE = 0x0002;
    private static final int RES_TABLE_PACKAGE_TYPE = 0x0200;
    private static final int RES_TABLE_TYPE_TYPE = 0x0201;

    private static final int UTF8_FLAG = 0x0100;
    private static final int TYPE_FLAG_SPARSE = 0x01;
    private static final int TYPE_FLAG_OFFSET16 = 0x02;
    private static final int TYPE_STRING = 0x03;

    static final class IconSlot {
        final String zipEntryPath;
        final int densityDpi;
        final boolean round;

        IconSlot(String zipEntryPath, int densityDpi, boolean round) {
            this.zipEntryPath = zipEntryPath;
            this.densityDpi = densityDpi;
            this.round = round;
        }
    }

    private ArscIconFinder() {}

    static Map<String, IconSlot> findIconSlots(byte[] arsc, int iconResId, int roundIconResId) {
        Map<String, IconSlot> result = new LinkedHashMap<>();
        if (arsc == null || arsc.length < 12) return result;

        ByteBuffer buf = ByteBuffer.wrap(arsc).order(ByteOrder.LITTLE_ENDIAN);
        int rootType = buf.getShort(0) & 0xFFFF;
        int rootHdr = buf.getShort(2) & 0xFFFF;
        if (rootType != RES_TABLE_TYPE || rootHdr < 12 || rootHdr > arsc.length) return result;

        List<String> globalPool = null;
        int pos = rootHdr;
        while (pos + 8 <= arsc.length) {
            int type = buf.getShort(pos) & 0xFFFF;
            int hdrSize = buf.getShort(pos + 2) & 0xFFFF;
            int chunkSize = buf.getInt(pos + 4);
            if (chunkSize < 8 || hdrSize < 8 || hdrSize > chunkSize || chunkSize > arsc.length - pos) break;

            if (type == RES_STRING_POOL_TYPE && globalPool == null && hdrSize >= 28) {
                globalPool = parseStringPool(arsc, pos, hdrSize, chunkSize);
            } else if (type == RES_TABLE_PACKAGE_TYPE && globalPool != null && hdrSize >= 12) {
                int pkgId = buf.getInt(pos + 8);
                int iconPkg = (iconResId >>> 24) & 0xFF;
                int roundPkg = (roundIconResId >>> 24) & 0xFF;
                if (pkgId == iconPkg || pkgId == roundPkg) {
                    scanPackage(arsc, pos, hdrSize, chunkSize, globalPool, iconResId, roundIconResId, result);
                }
            }
            pos += chunkSize;
        }
        return result;
    }

    private static void scanPackage(
            byte[] arsc,
            int pkgPos,
            int pkgHdrSize,
            int pkgChunkSize,
            List<String> globalPool,
            int iconResId,
            int roundIconResId,
            Map<String, IconSlot> out
    ) {
        ByteBuffer buf = ByteBuffer.wrap(arsc).order(ByteOrder.LITTLE_ENDIAN);
        int iconTypeId = (iconResId >>> 16) & 0xFF;
        int iconEntryId = iconResId & 0xFFFF;
        int roundTypeId = (roundIconResId >>> 16) & 0xFF;
        int roundEntryId = roundIconResId & 0xFFFF;

        int pos = pkgPos + pkgHdrSize;
        int pkgEnd = pkgPos + pkgChunkSize;
        while (pos + 8 <= pkgEnd) {
            int type = buf.getShort(pos) & 0xFFFF;
            int hdrSize = buf.getShort(pos + 2) & 0xFFFF;
            int chunkSize = buf.getInt(pos + 4);
            if (chunkSize < 8 || hdrSize < 8 || hdrSize > chunkSize || chunkSize > pkgEnd - pos) break;

            if (type == RES_TABLE_TYPE_TYPE && hdrSize >= 68) {
                int typeId = buf.get(pos + 8) & 0xFF;
                int typeFlags = buf.get(pos + 9) & 0xFF;
                int entryCount = buf.getInt(pos + 12);
                int entriesStart = buf.getInt(pos + 16);
                // ResTable_config begins at pos + 20: size(4) + imsi(4) + locale(4) + orient(1) + touch(1) -> density at +34.
                int density = buf.getShort(pos + 34) & 0xFFFF;

                if (entryCount > 0 && entriesStart >= hdrSize && entriesStart <= chunkSize
                        && (typeId == iconTypeId || typeId == roundTypeId)) {
                    int typeEnd = pos + chunkSize;
                    if (typeId == iconTypeId) {
                        extractEntryPath(buf, pos, typeEnd, hdrSize, entriesStart, entryCount, typeFlags,
                                iconEntryId, density, false, globalPool, out);
                    }
                    if (typeId == roundTypeId) {
                        extractEntryPath(buf, pos, typeEnd, hdrSize, entriesStart, entryCount, typeFlags,
                                roundEntryId, density, true, globalPool, out);
                    }
                }
            }
            pos += chunkSize;
        }
    }

    private static void extractEntryPath(
            ByteBuffer buf,
            int typePos,
            int typeEnd,
            int hdrSize,
            int entriesStart,
            int entryCount,
            int typeFlags,
            int targetEntryId,
            int density,
            boolean isRound,
            List<String> globalPool,
            Map<String, IconSlot> out
    ) {
        int relOffset = -1;
        int offsetsBase = typePos + hdrSize;
        int offsetsEnd = typePos + entriesStart;
        if ((typeFlags & TYPE_FLAG_SPARSE) != 0) {
            for (int i = 0; i < entryCount; i++) {
                long offPos = (long) offsetsBase + (long) i * 4L;
                if (offPos + 4 > offsetsEnd || offPos + 4 > typeEnd) break;
                int p = (int) offPos;
                int idx = buf.getShort(p) & 0xFFFF;
                if (idx == targetEntryId) {
                    relOffset = (buf.getShort(p + 2) & 0xFFFF) * 4;
                    break;
                }
            }
        } else if (targetEntryId >= 0 && targetEntryId < entryCount) {
            if ((typeFlags & TYPE_FLAG_OFFSET16) != 0) {
                long offPos = (long) offsetsBase + (long) targetEntryId * 2L;
                if (offPos + 2 > offsetsEnd || offPos + 2 > typeEnd) return;
                int off16 = buf.getShort((int) offPos) & 0xFFFF;
                if (off16 != 0xFFFF) relOffset = off16 * 4;
            } else {
                long offPos = (long) offsetsBase + (long) targetEntryId * 4L;
                if (offPos + 4 > offsetsEnd || offPos + 4 > typeEnd) return;
                relOffset = buf.getInt((int) offPos);
            }
        }
        if (relOffset < 0) return;

        long entryPosLong = (long) typePos + (long) entriesStart + (long) relOffset;
        if (entryPosLong < offsetsEnd || entryPosLong + 8 > typeEnd) return;
        int entryPos = (int) entryPosLong;
        int entrySize = buf.getShort(entryPos) & 0xFFFF;
        int entryFlags = buf.getShort(entryPos + 2) & 0xFFFF;
        if (entrySize < 8 || (entryFlags & 0x0001) != 0) return; // skip invalid or complex/bag entries

        long valPosLong = entryPosLong + (long) entrySize;
        if (valPosLong + 8 > typeEnd) return;
        int valPos = (int) valPosLong;
        int dataType = buf.get(valPos + 3) & 0xFF;
        int data = buf.getInt(valPos + 4);
        if (dataType == TYPE_STRING && data >= 0 && data < globalPool.size()) {
            String path = globalPool.get(data);
            if (path.endsWith(".png")) {
                out.put(path, new IconSlot(path, density, isRound));
            }
        }
    }

    private static List<String> parseStringPool(byte[] arsc, int poolPos, int hdrSize, int chunkSize) {
        ByteBuffer buf = ByteBuffer.wrap(arsc).order(ByteOrder.LITTLE_ENDIAN);
        int stringCount = buf.getInt(poolPos + 8);
        int flags = buf.getInt(poolPos + 16);
        int stringsStart = buf.getInt(poolPos + 20);
        boolean utf8 = (flags & UTF8_FLAG) != 0;

        if (stringCount < 0 || stringsStart < hdrSize || stringsStart > chunkSize
                || (long) hdrSize + (long) stringCount * 4L > chunkSize) {
            return new ArrayList<>();
        }

        List<String> list = new ArrayList<>(stringCount);
        int absStart = poolPos + stringsStart;
        int absEnd = poolPos + chunkSize;
        for (int i = 0; i < stringCount; i++) {
            int off = buf.getInt(poolPos + hdrSize + i * 4);
            if (off < 0 || off >= absEnd - absStart) {
                list.add("");
                continue;
            }
            int p = absStart + off;
            if (utf8) {
                if ((arsc[p] & 0x80) != 0) p += 2;
                else p += 1;
                if (p >= absEnd) {
                    list.add("");
                    continue;
                }
                int byteLen;
                if ((arsc[p] & 0x80) != 0) {
                    if (p + 1 >= absEnd) {
                        list.add("");
                        continue;
                    }
                    byteLen = ((arsc[p] & 0x7F) << 8) | (arsc[p + 1] & 0xFF);
                    p += 2;
                } else {
                    byteLen = arsc[p] & 0x7F;
                    p += 1;
                }
                int safeLen = Math.min(byteLen, Math.max(0, absEnd - p));
                list.add(new String(arsc, p, safeLen, StandardCharsets.UTF_8));
            } else {
                if (p + 2 > absEnd) {
                    list.add("");
                    continue;
                }
                int charLen = buf.getShort(p) & 0xFFFF;
                p += 2;
                if ((charLen & 0x8000) != 0) {
                    if (p + 2 > absEnd) {
                        list.add("");
                        continue;
                    }
                    charLen = ((charLen & 0x7FFF) << 16) | (buf.getShort(p) & 0xFFFF);
                    p += 2;
                }
                long byteLen = (long) charLen * 2L;
                int safeBytes = (int) Math.min(byteLen, Math.max(0, absEnd - p));
                list.add(new String(arsc, p, safeBytes, StandardCharsets.UTF_16LE));
            }
        }
        return list;
    }
}
