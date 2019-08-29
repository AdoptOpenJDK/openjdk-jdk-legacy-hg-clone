/*
 * Copyright (c) 2012, 2019, Oracle and/or its affiliates. All rights reserved.
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


package org.graalvm.compiler.hotspot.replacements;

import static jdk.vm.ci.meta.DeoptimizationAction.None;
import static jdk.vm.ci.meta.DeoptimizationReason.RuntimeConstraint;
import static org.graalvm.compiler.core.common.GraalOptions.GeneratePIC;
import static org.graalvm.compiler.core.common.GraalOptions.MinimalBulkZeroingSize;
import static org.graalvm.compiler.core.common.calc.UnsignedMath.belowThan;
import static org.graalvm.compiler.hotspot.GraalHotSpotVMConfigBase.INJECTED_OPTIONVALUES;
import static org.graalvm.compiler.hotspot.GraalHotSpotVMConfigBase.INJECTED_VMCONFIG;
import static org.graalvm.compiler.hotspot.HotSpotBackend.NEW_ARRAY;
import static org.graalvm.compiler.hotspot.HotSpotBackend.NEW_ARRAY_OR_NULL;
import static org.graalvm.compiler.hotspot.HotSpotBackend.NEW_INSTANCE;
import static org.graalvm.compiler.hotspot.HotSpotBackend.NEW_INSTANCE_OR_NULL;
import static org.graalvm.compiler.hotspot.HotSpotBackend.NEW_MULTI_ARRAY;
import static org.graalvm.compiler.hotspot.HotSpotBackend.NEW_MULTI_ARRAY_OR_NULL;
import static org.graalvm.compiler.hotspot.replacements.HotSpotReplacementsUtil.CLASS_ARRAY_KLASS_LOCATION;
import static org.graalvm.compiler.hotspot.replacements.HotSpotReplacementsUtil.CLASS_STATE_LOCATION;
import static org.graalvm.compiler.hotspot.replacements.HotSpotReplacementsUtil.HUB_WRITE_LOCATION;
import static org.graalvm.compiler.hotspot.replacements.HotSpotReplacementsUtil.MARK_WORD_LOCATION;
import static org.graalvm.compiler.hotspot.replacements.HotSpotReplacementsUtil.PROTOTYPE_MARK_WORD_LOCATION;
import static org.graalvm.compiler.hotspot.replacements.HotSpotReplacementsUtil.TLAB_END_LOCATION;
import static org.graalvm.compiler.hotspot.replacements.HotSpotReplacementsUtil.TLAB_TOP_LOCATION;
import static org.graalvm.compiler.hotspot.replacements.HotSpotReplacementsUtil.allocateInstancePrefetchLines;
import static org.graalvm.compiler.hotspot.replacements.HotSpotReplacementsUtil.allocatePrefetchDistance;
import static org.graalvm.compiler.hotspot.replacements.HotSpotReplacementsUtil.allocatePrefetchLines;
import static org.graalvm.compiler.hotspot.replacements.HotSpotReplacementsUtil.allocatePrefetchStepSize;
import static org.graalvm.compiler.hotspot.replacements.HotSpotReplacementsUtil.allocatePrefetchStyle;
import static org.graalvm.compiler.hotspot.replacements.HotSpotReplacementsUtil.arrayAllocationSize;
import static org.graalvm.compiler.hotspot.replacements.HotSpotReplacementsUtil.arrayKlassOffset;
import static org.graalvm.compiler.hotspot.replacements.HotSpotReplacementsUtil.arrayLengthOffset;
import static org.graalvm.compiler.hotspot.replacements.HotSpotReplacementsUtil.initializeObjectHeader;
import static org.graalvm.compiler.hotspot.replacements.HotSpotReplacementsUtil.instanceHeaderSize;
import static org.graalvm.compiler.hotspot.replacements.HotSpotReplacementsUtil.isInstanceKlassFullyInitialized;
import static org.graalvm.compiler.hotspot.replacements.HotSpotReplacementsUtil.layoutHelperHeaderSizeMask;
import static org.graalvm.compiler.hotspot.replacements.HotSpotReplacementsUtil.layoutHelperHeaderSizeShift;
import static org.graalvm.compiler.hotspot.replacements.HotSpotReplacementsUtil.layoutHelperLog2ElementSizeMask;
import static org.graalvm.compiler.hotspot.replacements.HotSpotReplacementsUtil.layoutHelperLog2ElementSizeShift;
import static org.graalvm.compiler.hotspot.replacements.HotSpotReplacementsUtil.loadKlassFromObject;
import static org.graalvm.compiler.hotspot.replacements.HotSpotReplacementsUtil.prototypeMarkWordOffset;
import static org.graalvm.compiler.hotspot.replacements.HotSpotReplacementsUtil.readLayoutHelper;
import static org.graalvm.compiler.hotspot.replacements.HotSpotReplacementsUtil.readTlabEnd;
import static org.graalvm.compiler.hotspot.replacements.HotSpotReplacementsUtil.readTlabTop;
import static org.graalvm.compiler.hotspot.replacements.HotSpotReplacementsUtil.registerAsWord;
import static org.graalvm.compiler.hotspot.replacements.HotSpotReplacementsUtil.useBiasedLocking;
import static org.graalvm.compiler.hotspot.replacements.HotSpotReplacementsUtil.useTLAB;
import static org.graalvm.compiler.hotspot.replacements.HotSpotReplacementsUtil.verifyOop;
import static org.graalvm.compiler.hotspot.replacements.HotSpotReplacementsUtil.writeTlabTop;
import static org.graalvm.compiler.hotspot.replacements.HotspotSnippetsOptions.ProfileAllocations;
import static org.graalvm.compiler.hotspot.replacements.HotspotSnippetsOptions.ProfileAllocationsContext;
import static org.graalvm.compiler.nodes.PiArrayNode.piArrayCastToSnippetReplaceeStamp;
import static org.graalvm.compiler.nodes.PiNode.piCastToSnippetReplaceeStamp;
import static org.graalvm.compiler.nodes.extended.BranchProbabilityNode.FAST_PATH_PROBABILITY;
import static org.graalvm.compiler.nodes.extended.BranchProbabilityNode.FREQUENT_PROBABILITY;
import static org.graalvm.compiler.nodes.extended.BranchProbabilityNode.SLOW_PATH_PROBABILITY;
import static org.graalvm.compiler.nodes.extended.BranchProbabilityNode.probability;
import static org.graalvm.compiler.replacements.ReplacementsUtil.REPLACEMENTS_ASSERTIONS_ENABLED;
import static org.graalvm.compiler.replacements.ReplacementsUtil.runtimeAssert;
import static org.graalvm.compiler.replacements.ReplacementsUtil.staticAssert;
import static org.graalvm.compiler.replacements.SnippetTemplate.DEFAULT_REPLACER;
import static org.graalvm.compiler.replacements.nodes.CStringConstant.cstring;
import static org.graalvm.compiler.replacements.nodes.ExplodeLoopNode.explodeLoop;

import org.graalvm.compiler.api.replacements.Fold;
import org.graalvm.compiler.api.replacements.Fold.InjectedParameter;
import org.graalvm.compiler.api.replacements.Snippet;
import org.graalvm.compiler.api.replacements.Snippet.ConstantParameter;
import org.graalvm.compiler.api.replacements.Snippet.VarargsParameter;
import org.graalvm.compiler.core.common.spi.ForeignCallDescriptor;
import org.graalvm.compiler.core.common.type.StampFactory;
import org.graalvm.compiler.debug.DebugHandlersFactory;
import org.graalvm.compiler.debug.GraalError;
import org.graalvm.compiler.graph.Node.ConstantNodeParameter;
import org.graalvm.compiler.graph.Node.NodeIntrinsic;
import org.graalvm.compiler.hotspot.GraalHotSpotVMConfig;
import org.graalvm.compiler.hotspot.meta.HotSpotProviders;
import org.graalvm.compiler.hotspot.meta.HotSpotRegistersProvider;
import org.graalvm.compiler.hotspot.nodes.DimensionsNode;
import org.graalvm.compiler.hotspot.nodes.aot.LoadConstantIndirectlyFixedNode;
import org.graalvm.compiler.hotspot.nodes.aot.LoadConstantIndirectlyNode;
import org.graalvm.compiler.hotspot.nodes.type.KlassPointerStamp;
import org.graalvm.compiler.hotspot.word.KlassPointer;
import org.graalvm.compiler.nodes.ConstantNode;
import org.graalvm.compiler.nodes.DeoptimizeNode;
import org.graalvm.compiler.nodes.PiNode;
import org.graalvm.compiler.nodes.PrefetchAllocateNode;
import org.graalvm.compiler.nodes.SnippetAnchorNode;
import org.graalvm.compiler.nodes.StructuredGraph;
import org.graalvm.compiler.nodes.ValueNode;
import org.graalvm.compiler.nodes.debug.DynamicCounterNode;
import org.graalvm.compiler.nodes.debug.VerifyHeapNode;
import org.graalvm.compiler.nodes.extended.ForeignCallNode;
import org.graalvm.compiler.nodes.extended.MembarNode;
import org.graalvm.compiler.nodes.java.DynamicNewArrayNode;
import org.graalvm.compiler.nodes.java.DynamicNewInstanceNode;
import org.graalvm.compiler.nodes.java.NewArrayNode;
import org.graalvm.compiler.nodes.java.NewInstanceNode;
import org.graalvm.compiler.nodes.java.NewMultiArrayNode;
import org.graalvm.compiler.nodes.memory.address.OffsetAddressNode;
import org.graalvm.compiler.nodes.spi.LoweringTool;
import org.graalvm.compiler.nodes.util.GraphUtil;
import org.graalvm.compiler.options.OptionValues;
import org.graalvm.compiler.replacements.ReplacementsUtil;
import org.graalvm.compiler.replacements.SnippetCounter;
import org.graalvm.compiler.replacements.SnippetCounter.Group;
import org.graalvm.compiler.replacements.SnippetTemplate;
import org.graalvm.compiler.replacements.SnippetTemplate.AbstractTemplates;
import org.graalvm.compiler.replacements.SnippetTemplate.Arguments;
import org.graalvm.compiler.replacements.SnippetTemplate.SnippetInfo;
import org.graalvm.compiler.replacements.Snippets;
import org.graalvm.compiler.replacements.nodes.ExplodeLoopNode;
import org.graalvm.compiler.replacements.nodes.ZeroMemoryNode;
import org.graalvm.compiler.word.Word;
import jdk.internal.vm.compiler.word.LocationIdentity;
import jdk.internal.vm.compiler.word.WordFactory;

import jdk.vm.ci.code.CodeUtil;
import jdk.vm.ci.code.MemoryBarriers;
import jdk.vm.ci.code.Register;
import jdk.vm.ci.code.TargetDescription;
import jdk.vm.ci.hotspot.HotSpotResolvedObjectType;
import jdk.vm.ci.meta.JavaKind;
import jdk.vm.ci.meta.ResolvedJavaType;

/**
 * Snippets used for implementing NEW, ANEWARRAY and NEWARRAY.
 */
public class NewObjectSnippets implements Snippets {

    enum ProfileContext {
        AllocatingMethod,
        InstanceOrArray,
        AllocatedType,
        AllocatedTypesInMethod,
        Total
    }

    @Fold
    static String createName(@Fold.InjectedParameter OptionValues options, String path, String typeContext) {
        switch (ProfileAllocationsContext.getValue(options)) {
            case AllocatingMethod:
                return "";
            case InstanceOrArray:
                return path;
            case AllocatedType:
            case AllocatedTypesInMethod:
                return typeContext;
            case Total:
                return "bytes";
            default:
                throw GraalError.shouldNotReachHere();
        }
    }

    @Fold
    static boolean doProfile(@Fold.InjectedParameter OptionValues options) {
        return ProfileAllocations.getValue(options);
    }

    @Fold
    static boolean withContext(@Fold.InjectedParameter OptionValues options) {
        ProfileContext context = ProfileAllocationsContext.getValue(options);
        return context == ProfileContext.AllocatingMethod || context == ProfileContext.AllocatedTypesInMethod;
    }

    protected static void profileAllocation(String path, long size, String typeContext) {
        if (doProfile(INJECTED_OPTIONVALUES)) {
            String name = createName(INJECTED_OPTIONVALUES, path, typeContext);

            boolean context = withContext(INJECTED_OPTIONVALUES);
            DynamicCounterNode.counter("number of bytes allocated", name, size, context);
            DynamicCounterNode.counter("number of allocations", name, 1, context);
        }
    }

    public static void emitPrefetchAllocate(Word address, boolean isArray) {
        if (allocatePrefetchStyle(INJECTED_VMCONFIG) > 0) {
            // Insert a prefetch for each allocation only on the fast-path
            // Generate several prefetch instructions.
            int lines = isArray ? allocatePrefetchLines(INJECTED_VMCONFIG) : allocateInstancePrefetchLines(INJECTED_VMCONFIG);
            int stepSize = allocatePrefetchStepSize(INJECTED_VMCONFIG);
            int distance = allocatePrefetchDistance(INJECTED_VMCONFIG);
            ExplodeLoopNode.explodeLoop();
            for (int i = 0; i < lines; i++) {
                PrefetchAllocateNode.prefetch(OffsetAddressNode.address(address, distance));
                distance += stepSize;
            }
        }
    }

    @Snippet
    public static Object allocateInstance(@ConstantParameter long size,
                    KlassPointer hub,
                    Word prototypeMarkWord,
                    @ConstantParameter boolean fillContents,
                    @ConstantParameter boolean emitMemoryBarrier,
                    @ConstantParameter Register threadRegister,
                    @ConstantParameter boolean constantSize,
                    @ConstantParameter String typeContext,
                    @ConstantParameter Counters counters) {
        return piCastToSnippetReplaceeStamp(allocateInstanceHelper(size, hub, prototypeMarkWord, fillContents, emitMemoryBarrier, threadRegister, constantSize, typeContext, counters));
    }

    public static Object allocateInstanceHelper(long size,
                    KlassPointer hub,
                    Word prototypeMarkWord,
                    boolean fillContents,
                    boolean emitMemoryBarrier,
                    Register threadRegister,
                    boolean constantSize,
                    String typeContext,
                    Counters counters) {
        Object result;
        Word thread = registerAsWord(threadRegister);
        Word top = readTlabTop(thread);
        Word end = readTlabEnd(thread);
        Word newTop = top.add(WordFactory.unsigned(size));
        if (useTLAB(INJECTED_VMCONFIG) && probability(FAST_PATH_PROBABILITY, newTop.belowOrEqual(end))) {
            writeTlabTop(thread, newTop);
            emitPrefetchAllocate(newTop, false);
            result = formatObject(hub, size, top, prototypeMarkWord, fillContents, emitMemoryBarrier, constantSize, counters);
        } else {
            Counters theCounters = counters;
            if (theCounters != null && theCounters.stub != null) {
                theCounters.stub.inc();
            }
            result = newInstanceStub(hub);
        }
        profileAllocation("instance", size, typeContext);
        return verifyOop(result);
    }

    public static Object newInstanceStub(KlassPointer hub) {
        if (useNullAllocationStubs(INJECTED_VMCONFIG)) {
            return nonNullOrDeopt(newInstanceOrNull(NEW_INSTANCE_OR_NULL, hub));
        } else {
            return newInstance(NEW_INSTANCE, hub);
        }
    }

    @NodeIntrinsic(value = ForeignCallNode.class, injectedStampIsNonNull = true)
    private static native Object newInstance(@ConstantNodeParameter ForeignCallDescriptor descriptor, KlassPointer hub);

    @NodeIntrinsic(value = ForeignCallNode.class, injectedStampIsNonNull = false)
    private static native Object newInstanceOrNull(@ConstantNodeParameter ForeignCallDescriptor descriptor, KlassPointer hub);

    @Snippet
    public static Object allocateInstancePIC(@ConstantParameter long size,
                    KlassPointer hub,
                    Word prototypeMarkWord,
                    @ConstantParameter boolean fillContents,
                    @ConstantParameter boolean emitMemoryBarrier,
                    @ConstantParameter Register threadRegister,
                    @ConstantParameter boolean constantSize,
                    @ConstantParameter String typeContext,
                    @ConstantParameter Counters counters) {
        // Klass must be initialized by the time the first instance is allocated, therefore we can
        // just load it from the corresponding cell and avoid the resolution check. We have to use a
        // fixed load though, to prevent it from floating above the initialization.
        KlassPointer picHub = LoadConstantIndirectlyFixedNode.loadKlass(hub);
        return piCastToSnippetReplaceeStamp(allocateInstanceHelper(size, picHub, prototypeMarkWord, fillContents, emitMemoryBarrier, threadRegister, constantSize, typeContext, counters));
    }

    @Snippet
    public static Object allocateInstanceDynamic(Class<?> type, Class<?> classClass,
                    @ConstantParameter boolean fillContents,
                    @ConstantParameter boolean emitMemoryBarrier,
                    @ConstantParameter Register threadRegister,
                    @ConstantParameter Counters counters) {
        if (probability(SLOW_PATH_PROBABILITY, type == null)) {
            DeoptimizeNode.deopt(None, RuntimeConstraint);
        }
        Class<?> nonNullType = PiNode.piCastNonNullClass(type, SnippetAnchorNode.anchor());

        if (probability(SLOW_PATH_PROBABILITY, DynamicNewInstanceNode.throwsInstantiationException(type, classClass))) {
            DeoptimizeNode.deopt(None, RuntimeConstraint);
        }

        return PiNode.piCastToSnippetReplaceeStamp(allocateInstanceDynamicHelper(type, fillContents, emitMemoryBarrier, threadRegister, counters, nonNullType));
    }

    private static Object allocateInstanceDynamicHelper(Class<?> type,
                    boolean fillContents,
                    boolean emitMemoryBarrier,
                    Register threadRegister,
                    Counters counters,
                    Class<?> nonNullType) {
        KlassPointer hub = ClassGetHubNode.readClass(nonNullType);
        if (probability(FAST_PATH_PROBABILITY, !hub.isNull())) {
            KlassPointer nonNullHub = ClassGetHubNode.piCastNonNull(hub, SnippetAnchorNode.anchor());

            if (probability(FAST_PATH_PROBABILITY, isInstanceKlassFullyInitialized(nonNullHub))) {
                int layoutHelper = readLayoutHelper(nonNullHub);
                /*
                 * src/share/vm/oops/klass.hpp: For instances, layout helper is a positive number,
                 * the instance size. This size is already passed through align_object_size and
                 * scaled to bytes. The low order bit is set if instances of this class cannot be
                 * allocated using the fastpath.
                 */
                if (probability(FAST_PATH_PROBABILITY, (layoutHelper & 1) == 0)) {
                    Word prototypeMarkWord = nonNullHub.readWord(prototypeMarkWordOffset(INJECTED_VMCONFIG), PROTOTYPE_MARK_WORD_LOCATION);
                    /*
                     * FIXME(je,ds): we should actually pass typeContext instead of "" but late
                     * binding of parameters is not yet supported by the GraphBuilderPlugin system.
                     */
                    return allocateInstanceHelper(layoutHelper, nonNullHub, prototypeMarkWord, fillContents, emitMemoryBarrier, threadRegister, false, "", counters);
                }
            } else {
                DeoptimizeNode.deopt(None, RuntimeConstraint);
            }
        }
        return dynamicNewInstanceStub(type);
    }

    /**
     * Maximum array length for which fast path allocation is used.
     */
    public static final int MAX_ARRAY_FAST_PATH_ALLOCATION_LENGTH = 0x00FFFFFF;

    @Snippet
    public static Object allocatePrimitiveArrayPIC(KlassPointer hub,
                    int length,
                    Word prototypeMarkWord,
                    @ConstantParameter int headerSize,
                    @ConstantParameter int log2ElementSize,
                    @ConstantParameter boolean fillContents,
                    @ConstantParameter boolean emitMemoryBarrier,
                    @ConstantParameter Register threadRegister,
                    @ConstantParameter boolean maybeUnroll,
                    @ConstantParameter String typeContext,
                    @ConstantParameter boolean useBulkZeroing,
                    @ConstantParameter Counters counters) {
        // Primitive array types are eagerly pre-resolved. We can use a floating load.
        KlassPointer picHub = LoadConstantIndirectlyNode.loadKlass(hub);
        return allocateArrayImpl(picHub, length, prototypeMarkWord, headerSize, log2ElementSize, fillContents,
                        emitMemoryBarrier, threadRegister, maybeUnroll, typeContext, useBulkZeroing, counters);
    }

    @Snippet
    public static Object allocateArrayPIC(KlassPointer hub,
                    int length,
                    Word prototypeMarkWord,
                    @ConstantParameter int headerSize,
                    @ConstantParameter int log2ElementSize,
                    @ConstantParameter boolean fillContents,
                    @ConstantParameter boolean emitMemoryBarrier,
                    @ConstantParameter Register threadRegister,
                    @ConstantParameter boolean maybeUnroll,
                    @ConstantParameter String typeContext,
                    @ConstantParameter boolean useBulkZeroing,
                    @ConstantParameter Counters counters) {
        // Array type would be resolved by dominating resolution.
        KlassPointer picHub = LoadConstantIndirectlyFixedNode.loadKlass(hub);
        return allocateArrayImpl(picHub, length, prototypeMarkWord, headerSize, log2ElementSize, fillContents,
                        emitMemoryBarrier, threadRegister, maybeUnroll, typeContext, useBulkZeroing, counters);
    }

    @Snippet
    public static Object allocateArray(KlassPointer hub,
                    int length,
                    Word prototypeMarkWord,
                    @ConstantParameter int headerSize,
                    @ConstantParameter int log2ElementSize,
                    @ConstantParameter boolean fillContents,
                    @ConstantParameter boolean emitMemoryBarrier,
                    @ConstantParameter Register threadRegister,
                    @ConstantParameter boolean maybeUnroll,
                    @ConstantParameter String typeContext,
                    @ConstantParameter boolean useBulkZeroing,
                    @ConstantParameter Counters counters) {
        Object result = allocateArrayImpl(hub,
                        length,
                        prototypeMarkWord,
                        headerSize,
                        log2ElementSize,
                        fillContents,
                        emitMemoryBarrier, threadRegister,
                        maybeUnroll,
                        typeContext,
                        useBulkZeroing,
                        counters);
        return piArrayCastToSnippetReplaceeStamp(verifyOop(result), length);
    }

    /**
     * When allocating on the slow path, determines whether to use a version of the runtime call
     * that returns {@code null} on a failed allocation instead of raising an OutOfMemoryError.
     */
    @Fold
    static boolean useNullAllocationStubs(@InjectedParameter GraalHotSpotVMConfig config) {
        return config.areNullAllocationStubsAvailable();
    }

    private static Object allocateArrayImpl(KlassPointer hub,
                    int length,
                    Word prototypeMarkWord,
                    int headerSize,
                    int log2ElementSize,
                    boolean fillContents,
                    boolean emitMemoryBarrier,
                    Register threadRegister,
                    boolean maybeUnroll,
                    String typeContext,
                    boolean useBulkZeroing,
                    Counters counters) {
        Object result;
        long allocationSize = arrayAllocationSize(length, headerSize, log2ElementSize);
        Word thread = registerAsWord(threadRegister);
        Word top = readTlabTop(thread);
        Word end = readTlabEnd(thread);
        Word newTop = top.add(WordFactory.unsigned(allocationSize));
        if (probability(FREQUENT_PROBABILITY, belowThan(length, MAX_ARRAY_FAST_PATH_ALLOCATION_LENGTH)) && useTLAB(INJECTED_VMCONFIG) &&
                        probability(FAST_PATH_PROBABILITY, newTop.belowOrEqual(end))) {
            writeTlabTop(thread, newTop);
            emitPrefetchAllocate(newTop, true);
            Counters theCounters = counters;
            if (theCounters != null && theCounters.arrayLoopInit != null) {
                theCounters.arrayLoopInit.inc();
            }
            result = formatArray(hub, allocationSize, length, headerSize, top, prototypeMarkWord, fillContents, emitMemoryBarrier, maybeUnroll, useBulkZeroing, counters);
        } else {
            result = newArrayStub(hub, length);
        }
        profileAllocation("array", allocationSize, typeContext);
        return result;
    }

    public static Object newArrayStub(KlassPointer hub, int length) {
        if (useNullAllocationStubs(INJECTED_VMCONFIG)) {
            return nonNullOrDeopt(newArrayOrNull(NEW_ARRAY_OR_NULL, hub, length));
        } else {
            return newArray(NEW_ARRAY, hub, length);
        }
    }

    @NodeIntrinsic(value = ForeignCallNode.class, injectedStampIsNonNull = true)
    private static native Object newArray(@ConstantNodeParameter ForeignCallDescriptor descriptor, KlassPointer hub, int length);

    @NodeIntrinsic(value = ForeignCallNode.class, injectedStampIsNonNull = false)
    private static native Object newArrayOrNull(@ConstantNodeParameter ForeignCallDescriptor descriptor, KlassPointer hub, int length);

    /**
     * New dynamic array stub that throws an {@link OutOfMemoryError} on allocation failure.
     */
    public static final ForeignCallDescriptor DYNAMIC_NEW_INSTANCE = new ForeignCallDescriptor("dynamic_new_instance", Object.class, Class.class);

    /**
     * New dynamic array stub that returns null on allocation failure.
     */
    public static final ForeignCallDescriptor DYNAMIC_NEW_INSTANCE_OR_NULL = new ForeignCallDescriptor("dynamic_new_instance_or_null", Object.class, Class.class);

    public static Object dynamicNewInstanceStub(Class<?> elementType) {
        if (useNullAllocationStubs(INJECTED_VMCONFIG)) {
            return nonNullOrDeopt(dynamicNewInstanceOrNull(DYNAMIC_NEW_INSTANCE_OR_NULL, elementType));
        } else {
            return dynamicNewInstance(DYNAMIC_NEW_INSTANCE, elementType);
        }
    }

    /**
     * Deoptimizes if {@code obj == null} otherwise returns {@code obj}.
     */
    private static Object nonNullOrDeopt(Object obj) {
        if (obj == null) {
            DeoptimizeNode.deopt(None, RuntimeConstraint);
        }
        return obj;
    }

    @NodeIntrinsic(value = ForeignCallNode.class, injectedStampIsNonNull = true)
    public static native Object dynamicNewInstance(@ConstantNodeParameter ForeignCallDescriptor descriptor, Class<?> elementType);

    @NodeIntrinsic(value = ForeignCallNode.class, injectedStampIsNonNull = false)
    public static native Object dynamicNewInstanceOrNull(@ConstantNodeParameter ForeignCallDescriptor descriptor, Class<?> elementType);

    @Snippet
    public static Object allocateArrayDynamic(Class<?> elementType,
                    Class<?> voidClass,
                    int length,
                    @ConstantParameter boolean fillContents,
                    @ConstantParameter boolean emitMemoryBarrier,
                    @ConstantParameter Register threadRegister,
                    @ConstantParameter JavaKind knownElementKind,
                    @ConstantParameter int knownLayoutHelper,
                    @ConstantParameter boolean useBulkZeroing,
                    Word prototypeMarkWord,
                    @ConstantParameter Counters counters) {
        Object result = allocateArrayDynamicImpl(elementType, voidClass, length, fillContents, emitMemoryBarrier, threadRegister, knownElementKind,
                        knownLayoutHelper, useBulkZeroing, prototypeMarkWord, counters);
        return result;
    }

    private static Object allocateArrayDynamicImpl(Class<?> elementType,
                    Class<?> voidClass,
                    int length,
                    boolean fillContents,
                    boolean emitMemoryBarrier,
                    Register threadRegister,
                    JavaKind knownElementKind,
                    int knownLayoutHelper,
                    boolean useBulkZeroing,
                    Word prototypeMarkWord,
                    Counters counters) {
        /*
         * We only need the dynamic check for void when we have no static information from
         * knownElementKind.
         */
        staticAssert(knownElementKind != JavaKind.Void, "unsupported knownElementKind");
        if (knownElementKind == JavaKind.Illegal && probability(SLOW_PATH_PROBABILITY, elementType == null || DynamicNewArrayNode.throwsIllegalArgumentException(elementType, voidClass))) {
            DeoptimizeNode.deopt(None, RuntimeConstraint);
        }

        KlassPointer klass = loadKlassFromObject(elementType, arrayKlassOffset(INJECTED_VMCONFIG), CLASS_ARRAY_KLASS_LOCATION);
        if (klass.isNull()) {
            DeoptimizeNode.deopt(None, RuntimeConstraint);
        }
        KlassPointer nonNullKlass = ClassGetHubNode.piCastNonNull(klass, SnippetAnchorNode.anchor());

        if (length < 0) {
            DeoptimizeNode.deopt(None, RuntimeConstraint);
        }
        int layoutHelper;
        if (knownElementKind == JavaKind.Illegal) {
            layoutHelper = readLayoutHelper(nonNullKlass);
        } else {
            runtimeAssert(knownLayoutHelper == readLayoutHelper(nonNullKlass), "layout mismatch");
            layoutHelper = knownLayoutHelper;
        }
        //@formatter:off
        // from src/share/vm/oops/klass.hpp:
        //
        // For arrays, layout helper is a negative number, containing four
        // distinct bytes, as follows:
        //    MSB:[tag, hsz, ebt, log2(esz)]:LSB
        // where:
        //    tag is 0x80 if the elements are oops, 0xC0 if non-oops
        //    hsz is array header size in bytes (i.e., offset of first element)
        //    ebt is the BasicType of the elements
        //    esz is the element size in bytes
        //@formatter:on

        int headerSize = (layoutHelper >> layoutHelperHeaderSizeShift(INJECTED_VMCONFIG)) & layoutHelperHeaderSizeMask(INJECTED_VMCONFIG);
        int log2ElementSize = (layoutHelper >> layoutHelperLog2ElementSizeShift(INJECTED_VMCONFIG)) & layoutHelperLog2ElementSizeMask(INJECTED_VMCONFIG);

        Object result = allocateArrayImpl(nonNullKlass, length, prototypeMarkWord, headerSize, log2ElementSize, fillContents,
                        emitMemoryBarrier, threadRegister, false, "dynamic type", useBulkZeroing, counters);
        return piArrayCastToSnippetReplaceeStamp(verifyOop(result), length);
    }

    /**
     * Calls the runtime stub for implementing MULTIANEWARRAY.
     */
    @Snippet
    private static Object newmultiarray(KlassPointer hub, @ConstantParameter int rank, @VarargsParameter int[] dimensions) {
        Word dims = DimensionsNode.allocaDimsArray(rank);
        ExplodeLoopNode.explodeLoop();
        for (int i = 0; i < rank; i++) {
            dims.writeInt(i * 4, dimensions[i], LocationIdentity.init());
        }
        return newMultiArrayStub(hub, rank, dims);
    }

    private static Object newMultiArrayStub(KlassPointer hub, int rank, Word dims) {
        if (useNullAllocationStubs(INJECTED_VMCONFIG)) {
            return nonNullOrDeopt(newMultiArrayOrNull(NEW_MULTI_ARRAY_OR_NULL, hub, rank, dims));
        } else {
            return newMultiArray(NEW_MULTI_ARRAY, hub, rank, dims);
        }
    }

    @Snippet
    private static Object newmultiarrayPIC(KlassPointer hub, @ConstantParameter int rank, @VarargsParameter int[] dimensions) {
        // Array type would be resolved by dominating resolution.
        KlassPointer picHub = LoadConstantIndirectlyFixedNode.loadKlass(hub);
        return newmultiarray(picHub, rank, dimensions);
    }

    @NodeIntrinsic(value = ForeignCallNode.class, injectedStampIsNonNull = true)
    private static native Object newMultiArray(@ConstantNodeParameter ForeignCallDescriptor descriptor, KlassPointer hub, int rank, Word dims);

    @NodeIntrinsic(value = ForeignCallNode.class, injectedStampIsNonNull = false)
    private static native Object newMultiArrayOrNull(@ConstantNodeParameter ForeignCallDescriptor descriptor, KlassPointer hub, int rank, Word dims);

    /**
     * Maximum number of long stores to emit when zeroing an object with a constant size. Larger
     * objects have their bodies initialized in a loop.
     */
    private static final int MAX_UNROLLED_OBJECT_ZEROING_STORES = 8;

    /**
     * Zero uninitialized memory in a newly allocated object, unrolling as necessary and ensuring
     * that stores are aligned.
     *
     * @param size number of bytes to zero
     * @param memory beginning of object which is being zeroed
     * @param constantSize is {@code size} known to be constant in the snippet
     * @param startOffset offset to begin zeroing. May not be word aligned.
     * @param manualUnroll maximally unroll zeroing
     * @param useBulkZeroing apply bulk zeroing
     */
    private static void zeroMemory(long size, Word memory, boolean constantSize, int startOffset, boolean manualUnroll,
                    boolean useBulkZeroing, Counters counters) {
        fillMemory(0, size, memory, constantSize, startOffset, manualUnroll, useBulkZeroing, counters);
    }

    private static void fillMemory(long value, long size, Word memory, boolean constantSize, int startOffset, boolean manualUnroll,
                    boolean useBulkZeroing, Counters counters) {
        ReplacementsUtil.runtimeAssert((size & 0x7) == 0, "unaligned object size");
        int offset = startOffset;
        if ((offset & 0x7) != 0) {
            memory.writeInt(offset, (int) value, LocationIdentity.init());
            offset += 4;
        }
        ReplacementsUtil.runtimeAssert((offset & 0x7) == 0, "unaligned offset");
        Counters theCounters = counters;
        if (manualUnroll && ((size - offset) / 8) <= MAX_UNROLLED_OBJECT_ZEROING_STORES) {
            ReplacementsUtil.staticAssert(!constantSize, "size shouldn't be constant at instantiation time");
            // This case handles arrays of constant length. Instead of having a snippet variant for
            // each length, generate a chain of stores of maximum length. Once it's inlined the
            // break statement will trim excess stores.
            if (theCounters != null && theCounters.instanceSeqInit != null) {
                theCounters.instanceSeqInit.inc();
            }

            explodeLoop();
            for (int i = 0; i < MAX_UNROLLED_OBJECT_ZEROING_STORES; i++, offset += 8) {
                if (offset == size) {
                    break;
                }
                memory.initializeLong(offset, value, LocationIdentity.init());
            }
        } else {
            // Use Word instead of int to avoid extension to long in generated code
            Word off = WordFactory.signed(offset);
            if (useBulkZeroing && value == 0 && probability(SLOW_PATH_PROBABILITY, (size - offset) >= getMinimalBulkZeroingSize(INJECTED_OPTIONVALUES))) {
                if (theCounters != null && theCounters.instanceBulkInit != null) {
                    theCounters.instanceBulkInit.inc();
                }
                ZeroMemoryNode.zero(memory.add(off), size - offset, LocationIdentity.init());
            } else {
                if (constantSize && ((size - offset) / 8) <= MAX_UNROLLED_OBJECT_ZEROING_STORES) {
                    if (theCounters != null && theCounters.instanceSeqInit != null) {
                        theCounters.instanceSeqInit.inc();
                    }
                    explodeLoop();
                } else {
                    if (theCounters != null && theCounters.instanceLoopInit != null) {
                        theCounters.instanceLoopInit.inc();
                    }
                }
                for (; off.rawValue() < size; off = off.add(8)) {
                    memory.initializeLong(off, value, LocationIdentity.init());
                }
            }
        }
    }

    @Fold
    static int getMinimalBulkZeroingSize(@InjectedParameter OptionValues optionValues) {
        return MinimalBulkZeroingSize.getValue(optionValues);
    }

    /**
     * Fill uninitialized memory with garbage value in a newly allocated object, unrolling as
     * necessary and ensuring that stores are aligned.
     *
     * @param size number of bytes to zero
     * @param memory beginning of object which is being zeroed
     * @param constantSize is {@code  size} known to be constant in the snippet
     * @param startOffset offset to begin zeroing. May not be word aligned.
     * @param manualUnroll maximally unroll zeroing
     */
    private static void fillWithGarbage(long size, Word memory, boolean constantSize, int startOffset, boolean manualUnroll, Counters counters) {
        fillMemory(0xfefefefefefefefeL, size, memory, constantSize, startOffset, manualUnroll, false, counters);
    }

    /**
     * Formats some allocated memory with an object header and zeroes out the rest.
     */
    private static Object formatObject(KlassPointer hub,
                    long size,
                    Word memory,
                    Word compileTimePrototypeMarkWord,
                    boolean fillContents,
                    boolean emitMemoryBarrier,
                    boolean constantSize,
                    Counters counters) {
        Word prototypeMarkWord = useBiasedLocking(INJECTED_VMCONFIG) ? hub.readWord(prototypeMarkWordOffset(INJECTED_VMCONFIG), PROTOTYPE_MARK_WORD_LOCATION) : compileTimePrototypeMarkWord;
        initializeObjectHeader(memory, prototypeMarkWord, hub);
        if (fillContents) {
            zeroMemory(size, memory, constantSize, instanceHeaderSize(INJECTED_VMCONFIG), false, false, counters);
        } else if (REPLACEMENTS_ASSERTIONS_ENABLED) {
            fillWithGarbage(size, memory, constantSize, instanceHeaderSize(INJECTED_VMCONFIG), false, counters);
        }
        if (emitMemoryBarrier) {
            MembarNode.memoryBarrier(MemoryBarriers.STORE_STORE, LocationIdentity.init());
        }
        return memory.toObjectNonNull();
    }

    @Snippet
    private static void verifyHeap(@ConstantParameter Register threadRegister) {
        Word thread = registerAsWord(threadRegister);
        Word topValue = readTlabTop(thread);
        if (!topValue.equal(WordFactory.zero())) {
            Word topValueContents = topValue.readWord(0, MARK_WORD_LOCATION);
            if (topValueContents.equal(WordFactory.zero())) {
                AssertionSnippets.vmMessageC(AssertionSnippets.ASSERTION_VM_MESSAGE_C, true, cstring("overzeroing of TLAB detected"), 0L, 0L, 0L);
            }
        }
    }

    /**
     * Formats some allocated memory with an object header and zeroes out the rest.
     */
    private static Object formatArray(KlassPointer hub,
                    long allocationSize,
                    int length,
                    int headerSize,
                    Word memory,
                    Word prototypeMarkWord,
                    boolean fillContents,
                    boolean emitMemoryBarrier,
                    boolean maybeUnroll,
                    boolean useBulkZeroing,
                    Counters counters) {
        memory.writeInt(arrayLengthOffset(INJECTED_VMCONFIG), length, LocationIdentity.init());
        /*
         * store hub last as the concurrent garbage collectors assume length is valid if hub field
         * is not null
         */
        initializeObjectHeader(memory, prototypeMarkWord, hub);
        if (fillContents) {
            zeroMemory(allocationSize, memory, false, headerSize, maybeUnroll, useBulkZeroing, counters);
        } else if (REPLACEMENTS_ASSERTIONS_ENABLED) {
            fillWithGarbage(allocationSize, memory, false, headerSize, maybeUnroll, counters);
        }
        if (emitMemoryBarrier) {
            MembarNode.memoryBarrier(MemoryBarriers.STORE_STORE, LocationIdentity.init());
        }
        return memory.toObjectNonNull();
    }

    static class Counters {
        Counters(SnippetCounter.Group.Factory factory) {
            Group newInstance = factory.createSnippetCounterGroup("NewInstance");
            Group newArray = factory.createSnippetCounterGroup("NewArray");
            instanceSeqInit = new SnippetCounter(newInstance, "tlabSeqInit", "TLAB alloc with unrolled zeroing");
            instanceLoopInit = new SnippetCounter(newInstance, "tlabLoopInit", "TLAB alloc with zeroing in a loop");
            instanceBulkInit = new SnippetCounter(newArray, "tlabBulkInit", "TLAB alloc with bulk zeroing");
            arrayLoopInit = new SnippetCounter(newArray, "tlabLoopInit", "TLAB alloc with zeroing in a loop");
            stub = new SnippetCounter(newInstance, "stub", "alloc and zeroing via stub");
        }

        final SnippetCounter instanceSeqInit;
        final SnippetCounter instanceLoopInit;
        final SnippetCounter instanceBulkInit;
        final SnippetCounter arrayLoopInit;
        final SnippetCounter stub;
    }

    public static class Templates extends AbstractTemplates {

        private final SnippetInfo allocateInstance = snippet(NewObjectSnippets.class, "allocateInstance", MARK_WORD_LOCATION, HUB_WRITE_LOCATION, TLAB_TOP_LOCATION, TLAB_END_LOCATION,
                        PROTOTYPE_MARK_WORD_LOCATION);
        private final SnippetInfo allocateInstancePIC = snippet(NewObjectSnippets.class, "allocateInstancePIC", MARK_WORD_LOCATION, HUB_WRITE_LOCATION, TLAB_TOP_LOCATION,
                        TLAB_END_LOCATION, PROTOTYPE_MARK_WORD_LOCATION);
        private final SnippetInfo allocateArray = snippet(NewObjectSnippets.class, "allocateArray", MARK_WORD_LOCATION, HUB_WRITE_LOCATION, TLAB_TOP_LOCATION, TLAB_END_LOCATION);
        private final SnippetInfo allocateArrayPIC = snippet(NewObjectSnippets.class, "allocateArrayPIC", MARK_WORD_LOCATION, HUB_WRITE_LOCATION, TLAB_TOP_LOCATION, TLAB_END_LOCATION);
        private final SnippetInfo allocatePrimitiveArrayPIC = snippet(NewObjectSnippets.class, "allocatePrimitiveArrayPIC", MARK_WORD_LOCATION, HUB_WRITE_LOCATION, TLAB_TOP_LOCATION,
                        TLAB_END_LOCATION);
        private final SnippetInfo allocateArrayDynamic = snippet(NewObjectSnippets.class, "allocateArrayDynamic", MARK_WORD_LOCATION, HUB_WRITE_LOCATION, TLAB_TOP_LOCATION,
                        TLAB_END_LOCATION);
        private final SnippetInfo allocateInstanceDynamic = snippet(NewObjectSnippets.class, "allocateInstanceDynamic", MARK_WORD_LOCATION, HUB_WRITE_LOCATION, TLAB_TOP_LOCATION,
                        TLAB_END_LOCATION, PROTOTYPE_MARK_WORD_LOCATION, CLASS_STATE_LOCATION);
        private final SnippetInfo newmultiarray = snippet(NewObjectSnippets.class, "newmultiarray", TLAB_TOP_LOCATION, TLAB_END_LOCATION);
        private final SnippetInfo newmultiarrayPIC = snippet(NewObjectSnippets.class, "newmultiarrayPIC", TLAB_TOP_LOCATION, TLAB_END_LOCATION);
        private final SnippetInfo verifyHeap = snippet(NewObjectSnippets.class, "verifyHeap");
        private final GraalHotSpotVMConfig config;
        private final Counters counters;

        public Templates(OptionValues options, Iterable<DebugHandlersFactory> factories, SnippetCounter.Group.Factory factory, HotSpotProviders providers, TargetDescription target,
                        GraalHotSpotVMConfig config) {
            super(options, factories, providers, providers.getSnippetReflection(), target);
            this.config = config;
            counters = new Counters(factory);
        }

        /**
         * Lowers a {@link NewInstanceNode}.
         */
        public void lower(NewInstanceNode newInstanceNode, HotSpotRegistersProvider registers, LoweringTool tool) {
            StructuredGraph graph = newInstanceNode.graph();
            HotSpotResolvedObjectType type = (HotSpotResolvedObjectType) newInstanceNode.instanceClass();
            assert !type.isArray();
            ConstantNode hub = ConstantNode.forConstant(KlassPointerStamp.klassNonNull(), type.klass(), providers.getMetaAccess(), graph);
            long size = instanceSize(type);

            OptionValues localOptions = graph.getOptions();
            SnippetInfo snippet = GeneratePIC.getValue(localOptions) ? allocateInstancePIC : allocateInstance;
            Arguments args = new Arguments(snippet, graph.getGuardsStage(), tool.getLoweringStage());
            args.addConst("size", size);
            args.add("hub", hub);
            args.add("prototypeMarkWord", type.prototypeMarkWord());
            args.addConst("fillContents", newInstanceNode.fillContents());
            args.addConst("emitMemoryBarrier", newInstanceNode.emitMemoryBarrier());
            args.addConst("threadRegister", registers.getThreadRegister());
            args.addConst("constantSize", true);
            args.addConst("typeContext", ProfileAllocations.getValue(localOptions) ? type.toJavaName(false) : "");
            args.addConst("counters", counters);

            SnippetTemplate template = template(newInstanceNode, args);
            graph.getDebug().log("Lowering allocateInstance in %s: node=%s, template=%s, arguments=%s", graph, newInstanceNode, template, args);
            template.instantiate(providers.getMetaAccess(), newInstanceNode, DEFAULT_REPLACER, args);
        }

        /**
         * Lowers a {@link NewArrayNode}.
         */
        public void lower(NewArrayNode newArrayNode, HotSpotRegistersProvider registers, LoweringTool tool) {
            StructuredGraph graph = newArrayNode.graph();
            ResolvedJavaType elementType = newArrayNode.elementType();
            HotSpotResolvedObjectType arrayType = (HotSpotResolvedObjectType) elementType.getArrayClass();
            JavaKind elementKind = elementType.getJavaKind();
            ConstantNode hub = ConstantNode.forConstant(KlassPointerStamp.klassNonNull(), arrayType.klass(), providers.getMetaAccess(), graph);
            final int headerSize = tool.getMetaAccess().getArrayBaseOffset(elementKind);
            int log2ElementSize = CodeUtil.log2(tool.getMetaAccess().getArrayIndexScale(elementKind));

            OptionValues localOptions = graph.getOptions();
            SnippetInfo snippet;
            if (GeneratePIC.getValue(localOptions)) {
                if (elementType.isPrimitive()) {
                    snippet = allocatePrimitiveArrayPIC;
                } else {
                    snippet = allocateArrayPIC;
                }
            } else {
                snippet = allocateArray;
            }

            Arguments args = new Arguments(snippet, graph.getGuardsStage(), tool.getLoweringStage());
            args.add("hub", hub);
            ValueNode length = newArrayNode.length();
            args.add("length", length.isAlive() ? length : graph.addOrUniqueWithInputs(length));
            assert arrayType.prototypeMarkWord() == lookupArrayClass(tool, JavaKind.Object).prototypeMarkWord() : "all array types are assumed to have the same prototypeMarkWord";
            args.add("prototypeMarkWord", arrayType.prototypeMarkWord());
            args.addConst("headerSize", headerSize);
            args.addConst("log2ElementSize", log2ElementSize);
            args.addConst("fillContents", newArrayNode.fillContents());
            args.addConst("emitMemoryBarrier", newArrayNode.emitMemoryBarrier());
            args.addConst("threadRegister", registers.getThreadRegister());
            args.addConst("maybeUnroll", length.isConstant());
            args.addConst("typeContext", ProfileAllocations.getValue(localOptions) ? arrayType.toJavaName(false) : "");
            args.addConst("useBulkZeroing", tool.getLowerer().supportBulkZeroing());
            args.addConst("counters", counters);
            SnippetTemplate template = template(newArrayNode, args);
            graph.getDebug().log("Lowering allocateArray in %s: node=%s, template=%s, arguments=%s", graph, newArrayNode, template, args);
            template.instantiate(providers.getMetaAccess(), newArrayNode, DEFAULT_REPLACER, args);
        }

        public void lower(DynamicNewInstanceNode newInstanceNode, HotSpotRegistersProvider registers, LoweringTool tool) {
            Arguments args = new Arguments(allocateInstanceDynamic, newInstanceNode.graph().getGuardsStage(), tool.getLoweringStage());
            args.add("type", newInstanceNode.getInstanceType());
            ValueNode classClass = newInstanceNode.getClassClass();
            assert classClass != null;
            args.add("classClass", classClass);
            args.addConst("fillContents", newInstanceNode.fillContents());
            args.addConst("emitMemoryBarrier", newInstanceNode.emitMemoryBarrier());
            args.addConst("threadRegister", registers.getThreadRegister());
            args.addConst("counters", counters);

            SnippetTemplate template = template(newInstanceNode, args);
            template.instantiate(providers.getMetaAccess(), newInstanceNode, DEFAULT_REPLACER, args);
        }

        public void lower(DynamicNewArrayNode newArrayNode, HotSpotRegistersProvider registers, LoweringTool tool) {
            StructuredGraph graph = newArrayNode.graph();
            Arguments args = new Arguments(allocateArrayDynamic, graph.getGuardsStage(), tool.getLoweringStage());
            args.add("elementType", newArrayNode.getElementType());
            ValueNode voidClass = newArrayNode.getVoidClass();
            assert voidClass != null;
            args.add("voidClass", voidClass);
            ValueNode length = newArrayNode.length();
            args.add("length", length.isAlive() ? length : graph.addOrUniqueWithInputs(length));
            args.addConst("fillContents", newArrayNode.fillContents());
            args.addConst("emitMemoryBarrier", newArrayNode.emitMemoryBarrier());
            args.addConst("threadRegister", registers.getThreadRegister());
            /*
             * We use Kind.Illegal as a marker value instead of null because constant snippet
             * parameters cannot be null.
             */
            args.addConst("knownElementKind", newArrayNode.getKnownElementKind() == null ? JavaKind.Illegal : newArrayNode.getKnownElementKind());
            if (newArrayNode.getKnownElementKind() != null) {
                args.addConst("knownLayoutHelper", lookupArrayClass(tool, newArrayNode.getKnownElementKind()).layoutHelper());
            } else {
                args.addConst("knownLayoutHelper", 0);
            }
            args.addConst("useBulkZeroing", tool.getLowerer().supportBulkZeroing());
            args.add("prototypeMarkWord", lookupArrayClass(tool, JavaKind.Object).prototypeMarkWord());
            args.addConst("counters", counters);
            SnippetTemplate template = template(newArrayNode, args);
            template.instantiate(providers.getMetaAccess(), newArrayNode, DEFAULT_REPLACER, args);
        }

        private static HotSpotResolvedObjectType lookupArrayClass(LoweringTool tool, JavaKind kind) {
            return (HotSpotResolvedObjectType) tool.getMetaAccess().lookupJavaType(kind == JavaKind.Object ? Object.class : kind.toJavaClass()).getArrayClass();
        }

        public void lower(NewMultiArrayNode newmultiarrayNode, LoweringTool tool) {
            StructuredGraph graph = newmultiarrayNode.graph();
            OptionValues localOptions = graph.getOptions();
            int rank = newmultiarrayNode.dimensionCount();
            ValueNode[] dims = new ValueNode[rank];
            for (int i = 0; i < newmultiarrayNode.dimensionCount(); i++) {
                dims[i] = newmultiarrayNode.dimension(i);
            }
            HotSpotResolvedObjectType type = (HotSpotResolvedObjectType) newmultiarrayNode.type();
            ConstantNode hub = ConstantNode.forConstant(KlassPointerStamp.klassNonNull(), type.klass(), providers.getMetaAccess(), graph);

            SnippetInfo snippet = GeneratePIC.getValue(localOptions) ? newmultiarrayPIC : newmultiarray;
            Arguments args = new Arguments(snippet, graph.getGuardsStage(), tool.getLoweringStage());
            args.add("hub", hub);
            args.addConst("rank", rank);
            args.addVarargs("dimensions", int.class, StampFactory.forKind(JavaKind.Int), dims);
            template(newmultiarrayNode, args).instantiate(providers.getMetaAccess(), newmultiarrayNode, DEFAULT_REPLACER, args);
        }

        private static long instanceSize(HotSpotResolvedObjectType type) {
            long size = type.instanceSize();
            assert size >= 0;
            return size;
        }

        public void lower(VerifyHeapNode verifyHeapNode, HotSpotRegistersProvider registers, LoweringTool tool) {
            if (config.cAssertions) {
                Arguments args = new Arguments(verifyHeap, verifyHeapNode.graph().getGuardsStage(), tool.getLoweringStage());
                args.addConst("threadRegister", registers.getThreadRegister());

                SnippetTemplate template = template(verifyHeapNode, args);
                template.instantiate(providers.getMetaAccess(), verifyHeapNode, DEFAULT_REPLACER, args);
            } else {
                GraphUtil.removeFixedWithUnusedInputs(verifyHeapNode);
            }
        }
    }
}
