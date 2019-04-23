/*
 * Copyright (c) 2015, 2018, Oracle and/or its affiliates. All rights reserved.
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


package org.graalvm.compiler.hotspot.meta;

import java.util.function.Supplier;

import org.graalvm.compiler.core.common.type.ObjectStamp;
import org.graalvm.compiler.core.common.type.Stamp;
import org.graalvm.compiler.core.common.type.StampFactory;
import org.graalvm.compiler.hotspot.nodes.aot.InitializeKlassNode;
import org.graalvm.compiler.hotspot.nodes.aot.ResolveConstantNode;
import org.graalvm.compiler.nodes.ConstantNode;
import org.graalvm.compiler.nodes.DeoptimizingFixedWithNextNode;
import org.graalvm.compiler.nodes.FrameState;
import org.graalvm.compiler.nodes.ValueNode;
import org.graalvm.compiler.nodes.graphbuilderconf.ClassInitializationPlugin;
import org.graalvm.compiler.nodes.graphbuilderconf.GraphBuilderContext;

import jdk.vm.ci.hotspot.HotSpotConstantPool;
import jdk.vm.ci.hotspot.HotSpotResolvedObjectType;
import jdk.vm.ci.meta.ConstantPool;
import jdk.vm.ci.meta.ResolvedJavaMethod;
import jdk.vm.ci.meta.ResolvedJavaType;

public final class HotSpotClassInitializationPlugin implements ClassInitializationPlugin {
    private static boolean shouldApply(GraphBuilderContext builder, ResolvedJavaType type) {
        if (!builder.parsingIntrinsic()) {
            if (!type.isArray()) {
                ResolvedJavaMethod method = builder.getGraph().method();
                ResolvedJavaType methodHolder = method.getDeclaringClass();
                // We can elide initialization nodes if type >=: methodHolder.
                // The type is already initialized by either "new" or "invokestatic".

                // Emit initialization node if type is an interface since:
                // JLS 12.4: Before a class is initialized, its direct superclass must be
                // initialized, but interfaces implemented by the class are not
                // initialized and a class or interface type T will be initialized
                // immediately before the first occurrence of accesses listed
                // in JLS 12.4.1.

                return !type.isAssignableFrom(methodHolder) || type.isInterface();
            } else if (!type.getComponentType().isPrimitive()) {
                // Always apply to object array types
                return true;
            }
        }
        return false;
    }

    @Override
    public boolean apply(GraphBuilderContext builder, ResolvedJavaType type, Supplier<FrameState> frameState, ValueNode[] classInit) {
        if (shouldApply(builder, type)) {
            Stamp hubStamp = builder.getStampProvider().createHubStamp((ObjectStamp) StampFactory.objectNonNull());
            ConstantNode hub = builder.append(ConstantNode.forConstant(hubStamp, ((HotSpotResolvedObjectType) type).klass(), builder.getMetaAccess(), builder.getGraph()));
            DeoptimizingFixedWithNextNode result = builder.append(type.isArray() ? new ResolveConstantNode(hub) : new InitializeKlassNode(hub));
            result.setStateBefore(frameState.get());
            if (classInit != null) {
                classInit[0] = result;
            }
            return true;
        }
        return false;
    }

    @Override
    public boolean supportsLazyInitialization(ConstantPool cp) {
        // jdk.vm.ci.hotspot.HotSpotConstantPool is final, so we can
        // directly compare Classes.
        return (cp instanceof HotSpotConstantPool);
    }

    @Override
    public void loadReferencedType(GraphBuilderContext builder, ConstantPool cp, int cpi, int opcode) {
        if (cp instanceof HotSpotConstantPool) {
            ((HotSpotConstantPool) cp).loadReferencedType(cpi, opcode, false);
        } else {
            cp.loadReferencedType(cpi, opcode);
        }
    }

}
