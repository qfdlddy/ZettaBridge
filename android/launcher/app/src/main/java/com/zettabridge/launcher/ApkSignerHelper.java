package com.zettabridge.launcher;

import android.content.Context;

import com.android.apksig.ApkSigner;
import com.android.apksig.apk.ApkFormatException;
import com.android.apksig.util.DataSources;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.math.BigInteger;
import java.nio.charset.StandardCharsets;
import java.security.GeneralSecurityException;
import java.security.KeyFactory;
import java.security.KeyPair;
import java.security.KeyPairGenerator;
import java.security.PrivateKey;
import java.security.SecureRandom;
import java.security.Signature;
import java.security.cert.CertificateFactory;
import java.security.cert.X509Certificate;
import java.security.spec.PKCS8EncodedKeySpec;
import java.util.Collections;

/**
 * Generates and persists a local RSA-2048 signing keypair + self-signed X.509 certificate in
 * pure Java (no {@code sun.security} dependency), then signs exported APKs with APK Signature
 * Scheme v1/v2/v3 via {@code apksig} using streaming file I/O.
 */
final class ApkSignerHelper {
    private static final String KEY_DIR = "export-signing";
    private static final String KEY_FILE = "key.pk8";
    private static final String CERT_FILE = "cert.der";
    private static final Object KEY_LOCK = new Object();

    // OID 1.2.840.113549.1.1.11 (sha256WithRSAEncryption) + NULL
    private static final byte[] SHA256_WITH_RSA_ALG_ID = new byte[] {
            0x30, 0x0d,
            0x06, 0x09, 0x2a, (byte) 0x86, 0x48, (byte) 0x86, (byte) 0xf7, 0x0d, 0x01, 0x01, 0x0b,
            0x05, 0x00
    };

    private ApkSignerHelper() {}

    static void signApk(Context context, File unsignedApk, File signedApk)
            throws IOException, GeneralSecurityException {
        File keyDir = new File(context.getFilesDir(), KEY_DIR);
        signApkWithKeyDir(keyDir, unsignedApk, signedApk);
    }

    static void signApkWithKeyDir(File keyDir, File unsignedApk, File signedApk)
            throws IOException, GeneralSecurityException {
        if (!keyDir.isDirectory() && !keyDir.mkdirs()) {
            throw new IOException("cannot create signing directory: " + keyDir);
        }
        File keyFile = new File(keyDir, KEY_FILE);
        File certFile = new File(keyDir, CERT_FILE);

        PrivateKey privateKey = null;
        X509Certificate cert = null;
        synchronized (KEY_LOCK) {
            if (keyFile.isFile() && certFile.isFile()) {
                try {
                    byte[] pk8 = java.nio.file.Files.readAllBytes(keyFile.toPath());
                    byte[] der = java.nio.file.Files.readAllBytes(certFile.toPath());
                    PrivateKey loadedKey = KeyFactory.getInstance("RSA").generatePrivate(new PKCS8EncodedKeySpec(pk8));
                    X509Certificate loadedCert = (X509Certificate) CertificateFactory.getInstance("X.509")
                            .generateCertificate(new ByteArrayInputStream(der));
                    if (keyMatchesCert(loadedKey, loadedCert)) {
                        privateKey = loadedKey;
                        cert = loadedCert;
                    }
                } catch (IOException | GeneralSecurityException ignored) {
                    // Corrupted or mismatched signing files; regenerate below.
                }
            }
            if (privateKey == null || cert == null) {
                KeyPairGenerator kpg = KeyPairGenerator.getInstance("RSA");
                kpg.initialize(2048, new SecureRandom());
                KeyPair kp = kpg.generateKeyPair();
                byte[] der = buildSelfSignedCert(kp, "ZettaBridge Export");
                privateKey = kp.getPrivate();
                cert = (X509Certificate) CertificateFactory.getInstance("X.509")
                        .generateCertificate(new ByteArrayInputStream(der));
                writeAtomic(keyFile, privateKey.getEncoded());
                writeAtomic(certFile, der);
            }
        }

        ApkSigner.SignerConfig signerConfig = new ApkSigner.SignerConfig.Builder(
                "zettabridge", privateKey, Collections.singletonList(cert))
                .build();
        try (RandomAccessFile raf = new RandomAccessFile(unsignedApk, "r")) {
            new ApkSigner.Builder(Collections.singletonList(signerConfig))
                    .setInputApk(DataSources.asDataSource(raf))
                    .setOutputApk(signedApk)
                    .setV1SigningEnabled(true)
                    .setV2SigningEnabled(true)
                    .setV3SigningEnabled(true)
                    .setMinSdkVersion(26)
                    .build()
                    .sign();
        } catch (ApkFormatException e) {
            throw new IOException("Invalid APK format while signing", e);
        }
    }

    static byte[] buildSelfSignedCert(KeyPair kp, String commonName) throws GeneralSecurityException {
        try {
            byte[] version = derTag(0xA0, derInteger(BigInteger.valueOf(2)));
            byte[] serialBytes = new byte[8];
            new SecureRandom().nextBytes(serialBytes);
            serialBytes[0] &= 0x7F;
            if (serialBytes[0] == 0) serialBytes[0] = 1;
            byte[] serial = derInteger(new BigInteger(1, serialBytes));

            // CN=<commonName>
            byte[] cnOid = new byte[] {0x06, 0x03, 0x55, 0x04, 0x03};
            byte[] cnVal = derTag(0x0C, commonName.getBytes(StandardCharsets.UTF_8));
            byte[] attr = derSequence(concat(cnOid, cnVal));
            byte[] rdn = derTag(0x31, attr);
            byte[] name = derSequence(rdn);

            byte[] notBefore = derTag(0x17, "240101000000Z".getBytes(StandardCharsets.US_ASCII));
            byte[] notAfter = derTag(0x17, "491231235959Z".getBytes(StandardCharsets.US_ASCII));
            byte[] validity = derSequence(concat(notBefore, notAfter));

            byte[] spki = kp.getPublic().getEncoded();

            byte[] tbs = derSequence(concat(
                    version, serial, SHA256_WITH_RSA_ALG_ID, name, validity, name, spki));

            Signature sig = Signature.getInstance("SHA256withRSA");
            sig.initSign(kp.getPrivate());
            sig.update(tbs);
            byte[] sigBytes = sig.sign();

            byte[] bitString = new byte[sigBytes.length + 1];
            bitString[0] = 0; // unused bits
            System.arraycopy(sigBytes, 0, bitString, 1, sigBytes.length);

            return derSequence(concat(tbs, SHA256_WITH_RSA_ALG_ID, derTag(0x03, bitString)));
        } catch (IOException e) {
            throw new GeneralSecurityException("Failed to encode X.509 certificate", e);
        }
    }

    private static byte[] derSequence(byte[] content) throws IOException {
        return derTag(0x30, content);
    }

    private static byte[] derInteger(BigInteger value) throws IOException {
        return derTag(0x02, value.toByteArray());
    }

    private static byte[] derTag(int tag, byte[] content) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream(content.length + 6);
        out.write(tag);
        int len = content.length;
        if (len < 0x80) {
            out.write(len);
        } else {
            int numBytes = 0;
            int tmp = len;
            while (tmp > 0) {
                numBytes++;
                tmp >>>= 8;
            }
            out.write(0x80 | numBytes);
            for (int i = numBytes - 1; i >= 0; i--) {
                out.write((len >>> (i * 8)) & 0xFF);
            }
        }
        out.write(content);
        return out.toByteArray();
    }

    private static byte[] concat(byte[]... parts) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        for (byte[] p : parts) out.write(p);
        return out.toByteArray();
    }

    private static boolean keyMatchesCert(PrivateKey privateKey, X509Certificate cert) {
        if (!(privateKey instanceof java.security.interfaces.RSAPrivateKey)
                || !(cert.getPublicKey() instanceof java.security.interfaces.RSAPublicKey)) {
            return false;
        }
        return ((java.security.interfaces.RSAPrivateKey) privateKey).getModulus()
                .equals(((java.security.interfaces.RSAPublicKey) cert.getPublicKey()).getModulus());
    }

    private static void writeAtomic(File dst, byte[] data) throws IOException {
        File parent = dst.getParentFile();
        File tmp = java.nio.file.Files.createTempFile(parent.toPath(), "." + dst.getName() + ".", ".tmp").toFile();
        boolean moved = false;
        try {
            try (FileOutputStream out = new FileOutputStream(tmp)) {
                out.write(data);
            }
            try {
                java.nio.file.Files.move(tmp.toPath(), dst.toPath(),
                        java.nio.file.StandardCopyOption.ATOMIC_MOVE,
                        java.nio.file.StandardCopyOption.REPLACE_EXISTING);
            } catch (java.nio.file.AtomicMoveNotSupportedException e) {
                java.nio.file.Files.move(tmp.toPath(), dst.toPath(),
                        java.nio.file.StandardCopyOption.REPLACE_EXISTING);
            }
            moved = true;
        } finally {
            if (!moved) tmp.delete();
        }
    }
}
