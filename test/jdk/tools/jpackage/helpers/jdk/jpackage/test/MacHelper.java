/*
 * Copyright (c) 2019, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */
package jdk.jpackage.test;

import java.io.ByteArrayInputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;
import java.util.Set;
import java.util.stream.Collectors;
import java.util.stream.Stream;
import javax.xml.parsers.DocumentBuilder;
import javax.xml.parsers.DocumentBuilderFactory;
import javax.xml.parsers.ParserConfigurationException;
import javax.xml.xpath.XPath;
import javax.xml.xpath.XPathConstants;
import javax.xml.xpath.XPathFactory;
import jdk.jpackage.test.Functional.ThrowingConsumer;
import jdk.jpackage.test.Functional.ThrowingSupplier;
import org.xml.sax.SAXException;

public class MacHelper {

    public static void withExplodedDmg(JPackageCommand cmd,
            ThrowingConsumer<Path> consumer) {
        cmd.verifyIsOfType(PackageType.MAC_DMG);

        var plist = readPList(new Executor()
                .setExecutable("/usr/bin/hdiutil")
                .dumpOutput()
                .addArguments("attach", cmd.outputBundle().toString(), "-plist")
                .executeAndGetOutput());

        final Path mountPoint = Path.of(plist.queryValue("mount-point"));
        try {
            Path dmgImage = mountPoint.resolve(cmd.name() + ".app");
            TKit.trace(String.format("Exploded [%s] in [%s] directory",
                    cmd.outputBundle(), dmgImage));
            ThrowingConsumer.toConsumer(consumer).accept(dmgImage);
        } finally {
            new Executor()
                    .setExecutable("/usr/bin/hdiutil")
                    .addArgument("detach").addArgument(mountPoint)
                    .execute().assertExitCodeIsZero();
        }
    }

    public static PListWrapper readPListFromAppImage(Path appImage) {
        return readPList(appImage.resolve("Contents/Info.plist"));
    }

    public static PListWrapper readPList(Path path) {
        TKit.assertReadableFileExists(path);
        return ThrowingSupplier.toSupplier(() -> readPList(Files.readAllLines(
                path))).get();
    }

    public static PListWrapper readPList(List<String> lines) {
        return readPList(lines.stream());
    }

    public static PListWrapper readPList(Stream<String> lines) {
        return ThrowingSupplier.toSupplier(() -> new PListWrapper(lines.collect(
                Collectors.joining()))).get();
    }

    static String getBundleName(JPackageCommand cmd) {
        cmd.verifyIsOfType(PackageType.MAC);
        return String.format("%s-%s%s", getPackageName(cmd), cmd.version(),
                cmd.packageType().getSuffix());
    }

    static Path getInstallationDirectory(JPackageCommand cmd) {
        cmd.verifyIsOfType(PackageType.MAC);
        return Path.of(cmd.getArgumentValue("--install-dir", () -> "/Applications"))
                .resolve(cmd.name() + ".app");
    }

    private static String getPackageName(JPackageCommand cmd) {
        return cmd.getArgumentValue("--mac-package-name",
                () -> cmd.name());
    }

    public static final class PListWrapper {
        public String queryValue(String keyName) {
            XPath xPath = XPathFactory.newInstance().newXPath();
            // Query for the value of <string> element preceding <key> element
            // with value equal to `keyName`
            String query = String.format(
                    "//string[preceding-sibling::key = \"%s\"][1]", keyName);
            return ThrowingSupplier.toSupplier(() -> (String) xPath.evaluate(
                    query, doc, XPathConstants.STRING)).get();
        }

        PListWrapper(String xml) throws ParserConfigurationException,
                SAXException, IOException {
            doc = createDocumentBuilder().parse(new ByteArrayInputStream(
                    xml.getBytes(StandardCharsets.UTF_8)));
        }

        private static DocumentBuilder createDocumentBuilder() throws
                ParserConfigurationException {
            DocumentBuilderFactory dbf = DocumentBuilderFactory.newDefaultInstance();
            dbf.setFeature(
                    "http://apache.org/xml/features/nonvalidating/load-external-dtd",
                    false);
            return dbf.newDocumentBuilder();
        }

        private final org.w3c.dom.Document doc;
    }

    static final Set<Path> CRITICAL_RUNTIME_FILES = Set.of(Path.of(
            "Contents/Home/lib/server/libjvm.dylib"));

}
