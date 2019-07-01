/*
 * Copyright (c) 2015, 2019, Oracle and/or its affiliates. All rights reserved.
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


package org.graalvm.compiler.replacements.amd64;

import static org.graalvm.compiler.replacements.StandardGraphBuilderPlugins.registerPlatformSpecificUnsafePlugins;
import static org.graalvm.compiler.replacements.nodes.BinaryMathIntrinsicNode.BinaryOperation.POW;
import static org.graalvm.compiler.replacements.nodes.UnaryMathIntrinsicNode.UnaryOperation.COS;
import static org.graalvm.compiler.replacements.nodes.UnaryMathIntrinsicNode.UnaryOperation.EXP;
import static org.graalvm.compiler.replacements.nodes.UnaryMathIntrinsicNode.UnaryOperation.LOG;
import static org.graalvm.compiler.replacements.nodes.UnaryMathIntrinsicNode.UnaryOperation.LOG10;
import static org.graalvm.compiler.replacements.nodes.UnaryMathIntrinsicNode.UnaryOperation.SIN;
import static org.graalvm.compiler.replacements.nodes.UnaryMathIntrinsicNode.UnaryOperation.TAN;

import java.util.Arrays;

import org.graalvm.compiler.bytecode.BytecodeProvider;
import org.graalvm.compiler.lir.amd64.AMD64ArithmeticLIRGeneratorTool.RoundingMode;
import org.graalvm.compiler.nodes.PauseNode;
import org.graalvm.compiler.nodes.ValueNode;
import org.graalvm.compiler.nodes.graphbuilderconf.GraphBuilderConfiguration.Plugins;
import org.graalvm.compiler.nodes.graphbuilderconf.GraphBuilderContext;
import org.graalvm.compiler.nodes.graphbuilderconf.InvocationPlugin;
import org.graalvm.compiler.nodes.graphbuilderconf.InvocationPlugin.Receiver;
import org.graalvm.compiler.nodes.graphbuilderconf.InvocationPlugins;
import org.graalvm.compiler.nodes.graphbuilderconf.InvocationPlugins.Registration;
import org.graalvm.compiler.nodes.java.AtomicReadAndAddNode;
import org.graalvm.compiler.nodes.java.AtomicReadAndWriteNode;
import org.graalvm.compiler.nodes.memory.address.OffsetAddressNode;
import org.graalvm.compiler.replacements.ArraysSubstitutions;
import org.graalvm.compiler.replacements.IntegerSubstitutions;
import org.graalvm.compiler.replacements.LongSubstitutions;
import org.graalvm.compiler.replacements.StandardGraphBuilderPlugins.UnsafeAccessPlugin;
import org.graalvm.compiler.replacements.StandardGraphBuilderPlugins.UnsafeGetPlugin;
import org.graalvm.compiler.replacements.StandardGraphBuilderPlugins.UnsafePutPlugin;
import org.graalvm.compiler.replacements.nodes.BinaryMathIntrinsicNode;
import org.graalvm.compiler.replacements.nodes.BinaryMathIntrinsicNode.BinaryOperation;
import org.graalvm.compiler.replacements.nodes.BitCountNode;
import org.graalvm.compiler.replacements.nodes.FusedMultiplyAddNode;
import org.graalvm.compiler.replacements.nodes.UnaryMathIntrinsicNode;
import org.graalvm.compiler.replacements.nodes.UnaryMathIntrinsicNode.UnaryOperation;
import org.graalvm.compiler.serviceprovider.JavaVersionUtil;

import jdk.vm.ci.amd64.AMD64;
import jdk.vm.ci.amd64.AMD64.CPUFeature;
import jdk.vm.ci.meta.JavaKind;
import jdk.vm.ci.meta.ResolvedJavaMethod;
import sun.misc.Unsafe;

public class AMD64GraphBuilderPlugins {

    public static void register(Plugins plugins, BytecodeProvider replacementsBytecodeProvider, AMD64 arch, boolean explicitUnsafeNullChecks, boolean emitJDK9StringSubstitutions,
                    boolean useFMAIntrinsics) {
        InvocationPlugins invocationPlugins = plugins.getInvocationPlugins();
        invocationPlugins.defer(new Runnable() {
            @Override
            public void run() {
                registerThreadPlugins(invocationPlugins, arch);
                registerIntegerLongPlugins(invocationPlugins, IntegerSubstitutions.class, JavaKind.Int, arch, replacementsBytecodeProvider);
                registerIntegerLongPlugins(invocationPlugins, LongSubstitutions.class, JavaKind.Long, arch, replacementsBytecodeProvider);
                registerPlatformSpecificUnsafePlugins(invocationPlugins, replacementsBytecodeProvider, explicitUnsafeNullChecks,
                                new JavaKind[]{JavaKind.Int, JavaKind.Long, JavaKind.Object, JavaKind.Boolean, JavaKind.Byte, JavaKind.Short, JavaKind.Char, JavaKind.Float, JavaKind.Double});
                registerUnsafePlugins(invocationPlugins, replacementsBytecodeProvider, explicitUnsafeNullChecks);
                registerStringPlugins(invocationPlugins, replacementsBytecodeProvider);
                if (emitJDK9StringSubstitutions) {
                    registerStringLatin1Plugins(invocationPlugins, replacementsBytecodeProvider);
                    registerStringUTF16Plugins(invocationPlugins, replacementsBytecodeProvider);
                }
                registerMathPlugins(invocationPlugins, useFMAIntrinsics, arch, replacementsBytecodeProvider);
                registerArraysEqualsPlugins(invocationPlugins, replacementsBytecodeProvider);
            }
        });
    }

    private static void registerThreadPlugins(InvocationPlugins plugins, AMD64 arch) {
        if (JavaVersionUtil.JAVA_SPEC > 8) {
            // Pause instruction introduced with SSE2
            if (arch.getFeatures().contains(AMD64.CPUFeature.SSE2)) {
                Registration r = new Registration(plugins, Thread.class);
                r.register0("onSpinWait", new InvocationPlugin() {
                    @Override
                    public boolean apply(GraphBuilderContext b, ResolvedJavaMethod targetMethod, Receiver receiver) {
                        b.append(new PauseNode());
                        return true;
                    }
                });
            }
        }
    }

    private static void registerIntegerLongPlugins(InvocationPlugins plugins, Class<?> substituteDeclaringClass, JavaKind kind, AMD64 arch, BytecodeProvider bytecodeProvider) {
        Class<?> declaringClass = kind.toBoxedJavaClass();
        Class<?> type = kind.toJavaClass();
        Registration r = new Registration(plugins, declaringClass, bytecodeProvider);
        r.registerMethodSubstitution(substituteDeclaringClass, "numberOfLeadingZeros", type);
        if (arch.getFeatures().contains(AMD64.CPUFeature.LZCNT) && arch.getFlags().contains(AMD64.Flag.UseCountLeadingZerosInstruction)) {
            r.setAllowOverwrite(true);
            r.register1("numberOfLeadingZeros", type, new InvocationPlugin() {
                @Override
                public boolean apply(GraphBuilderContext b, ResolvedJavaMethod targetMethod, Receiver receiver, ValueNode value) {
                    ValueNode folded = AMD64CountLeadingZerosNode.tryFold(value);
                    if (folded != null) {
                        b.addPush(JavaKind.Int, folded);
                    } else {
                        b.addPush(JavaKind.Int, new AMD64CountLeadingZerosNode(value));
                    }
                    return true;
                }
            });
        }

        r.registerMethodSubstitution(substituteDeclaringClass, "numberOfTrailingZeros", type);
        if (arch.getFeatures().contains(AMD64.CPUFeature.BMI1) && arch.getFlags().contains(AMD64.Flag.UseCountTrailingZerosInstruction)) {
            r.setAllowOverwrite(true);
            r.register1("numberOfTrailingZeros", type, new InvocationPlugin() {

                @Override
                public boolean apply(GraphBuilderContext b, ResolvedJavaMethod targetMethod, Receiver receiver, ValueNode value) {
                    ValueNode folded = AMD64CountTrailingZerosNode.tryFold(value);
                    if (folded != null) {
                        b.addPush(JavaKind.Int, folded);
                    } else {
                        b.addPush(JavaKind.Int, new AMD64CountTrailingZerosNode(value));
                    }
                    return true;
                }
            });
        }

        if (arch.getFeatures().contains(AMD64.CPUFeature.POPCNT)) {
            r.register1("bitCount", type, new InvocationPlugin() {
                @Override
                public boolean apply(GraphBuilderContext b, ResolvedJavaMethod targetMethod, Receiver receiver, ValueNode value) {
                    b.push(JavaKind.Int, b.append(new BitCountNode(value).canonical(null)));
                    return true;
                }
            });
        }

    }

    private static void registerMathPlugins(InvocationPlugins plugins, boolean useFMAIntrinsics, AMD64 arch, BytecodeProvider bytecodeProvider) {
        Registration r = new Registration(plugins, Math.class, bytecodeProvider);
        registerUnaryMath(r, "log", LOG);
        registerUnaryMath(r, "log10", LOG10);
        registerUnaryMath(r, "exp", EXP);
        registerBinaryMath(r, "pow", POW);
        registerUnaryMath(r, "sin", SIN);
        registerUnaryMath(r, "cos", COS);
        registerUnaryMath(r, "tan", TAN);

        if (arch.getFeatures().contains(CPUFeature.SSE4_1)) {
            registerRound(r, "rint", RoundingMode.NEAREST);
            registerRound(r, "ceil", RoundingMode.UP);
            registerRound(r, "floor", RoundingMode.DOWN);
        }

        if (useFMAIntrinsics && JavaVersionUtil.JAVA_SPEC > 8 && arch.getFeatures().contains(CPUFeature.FMA)) {
            registerFMA(r);
        }
    }

    private static void registerFMA(Registration r) {
        r.register3("fma",
                        Double.TYPE,
                        Double.TYPE,
                        Double.TYPE,
                        new InvocationPlugin() {
                            @Override
                            public boolean apply(GraphBuilderContext b,
                                            ResolvedJavaMethod targetMethod,
                                            Receiver receiver,
                                            ValueNode na,
                                            ValueNode nb,
                                            ValueNode nc) {
                                b.push(JavaKind.Double, b.append(new FusedMultiplyAddNode(na, nb, nc)));
                                return true;
                            }
                        });
        r.register3("fma",
                        Float.TYPE,
                        Float.TYPE,
                        Float.TYPE,
                        new InvocationPlugin() {
                            @Override
                            public boolean apply(GraphBuilderContext b,
                                            ResolvedJavaMethod targetMethod,
                                            Receiver receiver,
                                            ValueNode na,
                                            ValueNode nb,
                                            ValueNode nc) {
                                b.push(JavaKind.Float, b.append(new FusedMultiplyAddNode(na, nb, nc)));
                                return true;
                            }
                        });
    }

    private static void registerUnaryMath(Registration r, String name, UnaryOperation operation) {
        r.register1(name, Double.TYPE, new InvocationPlugin() {
            @Override
            public boolean apply(GraphBuilderContext b, ResolvedJavaMethod targetMethod, Receiver receiver, ValueNode value) {
                b.push(JavaKind.Double, b.append(UnaryMathIntrinsicNode.create(value, operation)));
                return true;
            }
        });
    }

    private static void registerBinaryMath(Registration r, String name, BinaryOperation operation) {
        r.register2(name, Double.TYPE, Double.TYPE, new InvocationPlugin() {
            @Override
            public boolean apply(GraphBuilderContext b, ResolvedJavaMethod targetMethod, Receiver receiver, ValueNode x, ValueNode y) {
                b.push(JavaKind.Double, b.append(BinaryMathIntrinsicNode.create(x, y, operation)));
                return true;
            }
        });
    }

    private static void registerRound(Registration r, String name, RoundingMode mode) {
        r.register1(name, Double.TYPE, new InvocationPlugin() {
            @Override
            public boolean apply(GraphBuilderContext b, ResolvedJavaMethod targetMethod, Receiver receiver, ValueNode arg) {
                b.push(JavaKind.Double, b.append(new AMD64RoundNode(arg, mode)));
                return true;
            }
        });
    }

    private static void registerStringPlugins(InvocationPlugins plugins, BytecodeProvider replacementsBytecodeProvider) {
        if (JavaVersionUtil.JAVA_SPEC <= 8) {
            Registration r;
            r = new Registration(plugins, String.class, replacementsBytecodeProvider);
            r.setAllowOverwrite(true);
            r.registerMethodSubstitution(AMD64StringSubstitutions.class, "indexOf", char[].class, int.class,
                            int.class, char[].class, int.class, int.class, int.class);
            r.registerMethodSubstitution(AMD64StringSubstitutions.class, "indexOf", Receiver.class, int.class, int.class);
            r.registerMethodSubstitution(AMD64StringSubstitutions.class, "compareTo", Receiver.class, String.class);
        }
    }

    private static void registerStringLatin1Plugins(InvocationPlugins plugins, BytecodeProvider replacementsBytecodeProvider) {
        Registration r = new Registration(plugins, "java.lang.StringLatin1", replacementsBytecodeProvider);
        r.setAllowOverwrite(true);
        r.registerMethodSubstitution(AMD64StringLatin1Substitutions.class, "compareTo", byte[].class, byte[].class);
        r.registerMethodSubstitution(AMD64StringLatin1Substitutions.class, "compareToUTF16", byte[].class, byte[].class);
        r.registerMethodSubstitution(AMD64StringLatin1Substitutions.class, "inflate", byte[].class, int.class, char[].class, int.class, int.class);
        r.registerMethodSubstitution(AMD64StringLatin1Substitutions.class, "inflate", byte[].class, int.class, byte[].class, int.class, int.class);
        r.registerMethodSubstitution(AMD64StringLatin1Substitutions.class, "indexOf", byte[].class, int.class, int.class);
        r.registerMethodSubstitution(AMD64StringLatin1Substitutions.class, "indexOf", byte[].class, int.class, byte[].class, int.class, int.class);
    }

    private static void registerStringUTF16Plugins(InvocationPlugins plugins, BytecodeProvider replacementsBytecodeProvider) {
        Registration r = new Registration(plugins, "java.lang.StringUTF16", replacementsBytecodeProvider);
        r.setAllowOverwrite(true);
        r.registerMethodSubstitution(AMD64StringUTF16Substitutions.class, "compareTo", byte[].class, byte[].class);
        r.registerMethodSubstitution(AMD64StringUTF16Substitutions.class, "compareToLatin1", byte[].class, byte[].class);
        r.registerMethodSubstitution(AMD64StringUTF16Substitutions.class, "compress", char[].class, int.class, byte[].class, int.class, int.class);
        r.registerMethodSubstitution(AMD64StringUTF16Substitutions.class, "compress", byte[].class, int.class, byte[].class, int.class, int.class);
        r.registerMethodSubstitution(AMD64StringUTF16Substitutions.class, "indexOfCharUnsafe", byte[].class, int.class, int.class, int.class);
        r.registerMethodSubstitution(AMD64StringUTF16Substitutions.class, "indexOfUnsafe", byte[].class, int.class, byte[].class, int.class, int.class);
        r.registerMethodSubstitution(AMD64StringUTF16Substitutions.class, "indexOfLatin1Unsafe", byte[].class, int.class, byte[].class, int.class, int.class);
    }

    private static void registerUnsafePlugins(InvocationPlugins plugins, BytecodeProvider replacementsBytecodeProvider, boolean explicitUnsafeNullChecks) {
        registerUnsafePlugins(new Registration(plugins, Unsafe.class), explicitUnsafeNullChecks, new JavaKind[]{JavaKind.Int, JavaKind.Long, JavaKind.Object}, true);
        if (JavaVersionUtil.JAVA_SPEC > 8) {
            registerUnsafePlugins(new Registration(plugins, "jdk.internal.misc.Unsafe", replacementsBytecodeProvider), explicitUnsafeNullChecks,
                            new JavaKind[]{JavaKind.Boolean, JavaKind.Byte, JavaKind.Char, JavaKind.Short, JavaKind.Int, JavaKind.Long, JavaKind.Object},
                            JavaVersionUtil.JAVA_SPEC <= 11);
        }
    }

    private static void registerUnsafePlugins(Registration r, boolean explicitUnsafeNullChecks, JavaKind[] unsafeJavaKinds, boolean java11OrEarlier) {
        for (JavaKind kind : unsafeJavaKinds) {
            Class<?> javaClass = kind == JavaKind.Object ? Object.class : kind.toJavaClass();
            String kindName = (kind == JavaKind.Object && !java11OrEarlier) ? "Reference" : kind.name();
            r.register4("getAndSet" + kindName, Receiver.class, Object.class, long.class, javaClass, new UnsafeAccessPlugin(kind, explicitUnsafeNullChecks) {
                @Override
                public boolean apply(GraphBuilderContext b, ResolvedJavaMethod targetMethod, Receiver unsafe, ValueNode object, ValueNode offset, ValueNode value) {
                    // Emits a null-check for the otherwise unused receiver
                    unsafe.get();
                    createUnsafeAccess(object, b, (obj, loc) -> new AtomicReadAndWriteNode(obj, offset, value, kind, loc));
                    return true;
                }
            });
            if (kind != JavaKind.Boolean && kind.isNumericInteger()) {
                r.register4("getAndAdd" + kindName, Receiver.class, Object.class, long.class, javaClass, new UnsafeAccessPlugin(kind, explicitUnsafeNullChecks) {
                    @Override
                    public boolean apply(GraphBuilderContext b, ResolvedJavaMethod targetMethod, Receiver unsafe, ValueNode object, ValueNode offset, ValueNode delta) {
                        // Emits a null-check for the otherwise unused receiver
                        unsafe.get();
                        createUnsafeAccess(object, b, (obj, loc) -> new AtomicReadAndAddNode(b.add(new OffsetAddressNode(obj, offset)), delta, kind, loc));
                        return true;
                    }
                });
            }
        }

        for (JavaKind kind : new JavaKind[]{JavaKind.Char, JavaKind.Short, JavaKind.Int, JavaKind.Long}) {
            Class<?> javaClass = kind.toJavaClass();
            r.registerOptional3("get" + kind.name() + "Unaligned", Receiver.class, Object.class, long.class, new UnsafeGetPlugin(kind, explicitUnsafeNullChecks));
            r.registerOptional4("put" + kind.name() + "Unaligned", Receiver.class, Object.class, long.class, javaClass, new UnsafePutPlugin(kind, explicitUnsafeNullChecks));
        }
    }

    private static void registerArraysEqualsPlugins(InvocationPlugins plugins, BytecodeProvider bytecodeProvider) {
        Registration r = new Registration(plugins, Arrays.class, bytecodeProvider);
        r.registerMethodSubstitution(ArraysSubstitutions.class, "equals", float[].class, float[].class);
        r.registerMethodSubstitution(ArraysSubstitutions.class, "equals", double[].class, double[].class);
    }
}
