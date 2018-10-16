/*
 * Copyright (c) 2013, 2018, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
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

package sun.reflect.annotation;

import java.lang.annotation.*;
import java.lang.reflect.*;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.StringJoiner;

import static sun.reflect.annotation.TypeAnnotation.*;

public final class AnnotatedTypeFactory {
    /**
     * Create an AnnotatedType.
     *
     * @param type the type this AnnotatedType corresponds to
     * @param currentLoc the location this AnnotatedType corresponds to
     * @param actualTypeAnnos the type annotations this AnnotatedType has
     * @param allOnSameTarget all type annotation on the same TypeAnnotationTarget
     *                          as the AnnotatedType being built
     * @param decl the declaration having the type use this AnnotatedType
     *                          corresponds to
     */
    public static AnnotatedType buildAnnotatedType(Type type,
            LocationInfo currentLoc,
            TypeAnnotation[] actualTypeAnnos,
            TypeAnnotation[] allOnSameTarget,
            AnnotatedElement decl) {
        if (type == null) {
            return EMPTY_ANNOTATED_TYPE;
        }
        if (isArray(type))
            return new AnnotatedArrayTypeImpl(type,
                    currentLoc,
                    actualTypeAnnos,
                    allOnSameTarget,
                    decl);
        if (type instanceof Class) {
            return new AnnotatedTypeBaseImpl(type,
                    currentLoc,
                    actualTypeAnnos,
                    allOnSameTarget,
                    decl);
        } else if (type instanceof TypeVariable) {
            return new AnnotatedTypeVariableImpl((TypeVariable)type,
                    currentLoc,
                    actualTypeAnnos,
                    allOnSameTarget,
                    decl);
        } else if (type instanceof ParameterizedType) {
            return new AnnotatedParameterizedTypeImpl((ParameterizedType)type,
                    currentLoc,
                    actualTypeAnnos,
                    allOnSameTarget,
                    decl);
        } else if (type instanceof WildcardType) {
            return new AnnotatedWildcardTypeImpl((WildcardType) type,
                    currentLoc,
                    actualTypeAnnos,
                    allOnSameTarget,
                    decl);
        }
        throw new AssertionError("Unknown instance of Type: " + type + "\nThis should not happen.");
    }

    public static LocationInfo nestingForType(Type type, LocationInfo addTo) {
        if (isArray(type))
            return addTo;
        if (type instanceof Class) {
            Class<?> clz = (Class)type;
            if (clz.getEnclosingClass() == null)
                return addTo;
            if (Modifier.isStatic(clz.getModifiers()))
                return nestingForType(clz.getEnclosingClass(), addTo);
            return nestingForType(clz.getEnclosingClass(), addTo.pushInner());
        } else if (type instanceof ParameterizedType) {
            ParameterizedType t = (ParameterizedType)type;
            if (t.getOwnerType() == null)
                return addTo;
            return nestingForType(t.getOwnerType(), addTo.pushInner());
        }
        return addTo;
    }

    private static boolean isArray(Type t) {
        if (t instanceof Class) {
            Class<?> c = (Class)t;
            if (c.isArray())
                return true;
        } else if (t instanceof GenericArrayType) {
            return true;
        }
        return false;
    }

    static final TypeAnnotation[] EMPTY_TYPE_ANNOTATION_ARRAY = new TypeAnnotation[0];
    static final AnnotatedType EMPTY_ANNOTATED_TYPE = new AnnotatedTypeBaseImpl(null, LocationInfo.BASE_LOCATION,
            EMPTY_TYPE_ANNOTATION_ARRAY, EMPTY_TYPE_ANNOTATION_ARRAY, null);
    static final AnnotatedType[] EMPTY_ANNOTATED_TYPE_ARRAY = new AnnotatedType[0];

    private static class AnnotatedTypeBaseImpl implements AnnotatedType {
        private final Type type;
        private final AnnotatedElement decl;
        private final LocationInfo location;
        private final TypeAnnotation[] allOnSameTargetTypeAnnotations;
        private final Map<Class <? extends Annotation>, Annotation> annotations;

        AnnotatedTypeBaseImpl(Type type, LocationInfo location,
                TypeAnnotation[] actualTypeAnnotations, TypeAnnotation[] allOnSameTargetTypeAnnotations,
                AnnotatedElement decl) {
            this.type = type;
            this.decl = decl;
            this.location = location;
            this.allOnSameTargetTypeAnnotations = allOnSameTargetTypeAnnotations;
            this.annotations = TypeAnnotationParser.mapTypeAnnotations(location.filter(actualTypeAnnotations));
        }

        // AnnotatedElement
        @Override
        public final Annotation[] getAnnotations() {
            return getDeclaredAnnotations();
        }

        @Override
        public final <T extends Annotation> T getAnnotation(Class<T> annotation) {
            return getDeclaredAnnotation(annotation);
        }

        @Override
        public final <T extends Annotation> T[] getAnnotationsByType(Class<T> annotation) {
            return getDeclaredAnnotationsByType(annotation);
        }

        @Override
        public final Annotation[] getDeclaredAnnotations() {
            return annotations.values().toArray(new Annotation[0]);
        }

        @Override
        @SuppressWarnings("unchecked")
        public final <T extends Annotation> T getDeclaredAnnotation(Class<T> annotation) {
            return (T)annotations.get(annotation);
        }

        @Override
        public final <T extends Annotation> T[] getDeclaredAnnotationsByType(Class<T> annotation) {
            return AnnotationSupport.getDirectlyAndIndirectlyPresent(annotations, annotation);
        }

        // AnnotatedType
        @Override
        public final Type getType() {
            return type;
        }

        @Override
        public AnnotatedType getAnnotatedOwnerType() {
            if (!(type instanceof Class<?>))
                throw new IllegalStateException("Can't compute owner");

            Class<?> inner = (Class<?>)type;
            Class<?> owner = inner.getDeclaringClass();
            if (owner == null) // top-level, local or anonymous
                return null;
            if (inner.isPrimitive() || inner == Void.TYPE)
                return null;

            LocationInfo outerLoc = nestingForType(owner, getLocation().popAllLocations((byte)1));
            TypeAnnotation[]all = getTypeAnnotations();
            List<TypeAnnotation> l = new ArrayList<>(all.length);

            for (TypeAnnotation t : all)
                if (t.getLocationInfo().isSameLocationInfo(outerLoc))
                    l.add(t);

            return buildAnnotatedType(owner, outerLoc, l.toArray(EMPTY_TYPE_ANNOTATION_ARRAY), all, getDecl());

        }

        @Override // java.lang.Object
        public String toString() {
            // Reusable toString implementation, but needs to be
            // specialized for quirks of arrays.
            return annotationsToString(getAnnotations(), false) + type.toString();
        }

        protected String annotationsToString(Annotation[] annotations, boolean leadingSpace) {
            if (annotations != null && annotations.length > 0) {
                StringJoiner sj = new StringJoiner(" ");
                if (leadingSpace) {
                    sj.add(""); // Add a space
                }

                for (Annotation annotation : annotations) {
                    sj.add(annotation.toString());
                }

                if (!leadingSpace) {
                    sj.add("");
                }
                return sj.toString();
            } else {
                return "";
            }
        }

        protected boolean equalsTypeAndAnnotations(AnnotatedType that) {
            return getType().equals(that.getType()) &&
                // Treat ordering of annotations as significant
                Arrays.equals(getAnnotations(), that.getAnnotations()) &&
                Objects.equals(getAnnotatedOwnerType(), that.getAnnotatedOwnerType());
        }

        int baseHashCode() {
            return type.hashCode() ^
                // Acceptable to use Objects.hash rather than
                // Arrays.deepHashCode since the elements of the array
                // are not themselves arrays.
                Objects.hash((Object[])getAnnotations()) ^
                Objects.hash(getAnnotatedOwnerType());
        }

        @Override
        public boolean equals(Object o) {
            if (o instanceof AnnotatedType &&
                !(o instanceof AnnotatedArrayType) &&
                !(o instanceof AnnotatedTypeVariable) &&
                !(o instanceof AnnotatedParameterizedType) &&
                !(o instanceof AnnotatedWildcardType)) {
                AnnotatedType that = (AnnotatedType) o;
                return equalsTypeAndAnnotations(that);
            } else {
                return false;
            }
        }

        @Override
        public int hashCode() {
            return baseHashCode();
        }

        // Implementation details
        final LocationInfo getLocation() {
            return location;
        }
        final TypeAnnotation[] getTypeAnnotations() {
            return allOnSameTargetTypeAnnotations;
        }
        final AnnotatedElement getDecl() {
            return decl;
        }
    }

    private static final class AnnotatedArrayTypeImpl extends AnnotatedTypeBaseImpl implements AnnotatedArrayType {
        AnnotatedArrayTypeImpl(Type type, LocationInfo location,
                TypeAnnotation[] actualTypeAnnotations, TypeAnnotation[] allOnSameTargetTypeAnnotations,
                AnnotatedElement decl) {
            super(type, location, actualTypeAnnotations, allOnSameTargetTypeAnnotations, decl);
        }

        @Override
        public AnnotatedType getAnnotatedGenericComponentType() {
            Type t = getComponentType();
            return AnnotatedTypeFactory.buildAnnotatedType(t,
                    nestingForType(t, getLocation().pushArray()),
                    getTypeAnnotations(),
                    getTypeAnnotations(),
                    getDecl());
        }

        @Override
        public AnnotatedType getAnnotatedOwnerType() {
            return null;
        }

        private Type getComponentType() {
            Type t = getType();
            if (t instanceof Class) {
                Class<?> c = (Class)t;
                return c.getComponentType();
            }
            return ((GenericArrayType)t).getGenericComponentType();
        }

        @Override
        public String toString() {
            // To annotate the full type of an array, the annotations
            // are placed between the type and the brackets. For
            // example, to annotate an array of Strings, the syntax used is
            //
            // String @TypeAnnotation []
            //
            // and *not*
            //
            // @TypeAnnotation String[].
            //
            // The toString output should strive to be reusable in
            // source code. Therefore, the general logic of putting
            // the annotations before a textual representation of the
            // type need to be overridden for arrays.
            StringBuilder sb = new StringBuilder();

            AnnotatedType componentType = this;
            while (componentType instanceof AnnotatedArrayType) {
                AnnotatedArrayType annotatedArrayType = (AnnotatedArrayType) componentType;
                sb.append(annotationsToString(annotatedArrayType.getAnnotations(), true) + "[]");
                componentType = annotatedArrayType.getAnnotatedGenericComponentType();
            }

            sb.insert(0, componentType.toString());
            return sb.toString();
        }

        @Override
        public boolean equals(Object o) {
            if (o instanceof AnnotatedArrayType) {
                AnnotatedArrayType that = (AnnotatedArrayType) o;
                return equalsTypeAndAnnotations(that) &&
                    Objects.equals(getAnnotatedGenericComponentType(),
                                   that.getAnnotatedGenericComponentType());
            } else {
                return false;
            }
        }

        @Override
        public int hashCode() {
            return baseHashCode() ^ getAnnotatedGenericComponentType().hashCode();
        }
    }

    private static final class AnnotatedTypeVariableImpl extends AnnotatedTypeBaseImpl implements AnnotatedTypeVariable {
        AnnotatedTypeVariableImpl(TypeVariable<?> type, LocationInfo location,
                TypeAnnotation[] actualTypeAnnotations, TypeAnnotation[] allOnSameTargetTypeAnnotations,
                AnnotatedElement decl) {
            super(type, location, actualTypeAnnotations, allOnSameTargetTypeAnnotations, decl);
        }

        @Override
        public AnnotatedType[] getAnnotatedBounds() {
            return getTypeVariable().getAnnotatedBounds();
        }

        @Override
        public AnnotatedType getAnnotatedOwnerType() {
            return null;
        }

        private TypeVariable<?> getTypeVariable() {
            return (TypeVariable)getType();
        }

        @Override
        public boolean equals(Object o) {
            if (o instanceof AnnotatedTypeVariable) {
                AnnotatedTypeVariable that = (AnnotatedTypeVariable) o;
                return equalsTypeAndAnnotations(that) &&
                    Arrays.equals(getAnnotatedBounds(), that.getAnnotatedBounds());
            } else {
                return false;
            }
        }

        @Override
        public int hashCode() {
            return baseHashCode() ^
                Objects.hash((Object[])getAnnotatedBounds());
        }
    }

    private static final class AnnotatedParameterizedTypeImpl extends AnnotatedTypeBaseImpl
            implements AnnotatedParameterizedType {
        AnnotatedParameterizedTypeImpl(ParameterizedType type, LocationInfo location,
                TypeAnnotation[] actualTypeAnnotations, TypeAnnotation[] allOnSameTargetTypeAnnotations,
                AnnotatedElement decl) {
            super(type, location, actualTypeAnnotations, allOnSameTargetTypeAnnotations, decl);
        }

        @Override
        public AnnotatedType[] getAnnotatedActualTypeArguments() {
            Type[] arguments = getParameterizedType().getActualTypeArguments();
            AnnotatedType[] res = new AnnotatedType[arguments.length];
            Arrays.fill(res, EMPTY_ANNOTATED_TYPE);
            int initialCapacity = getTypeAnnotations().length;
            for (int i = 0; i < res.length; i++) {
                List<TypeAnnotation> l = new ArrayList<>(initialCapacity);
                LocationInfo newLoc = nestingForType(arguments[i], getLocation().pushTypeArg((byte)i));
                for (TypeAnnotation t : getTypeAnnotations())
                    if (t.getLocationInfo().isSameLocationInfo(newLoc))
                        l.add(t);
                res[i] = buildAnnotatedType(arguments[i],
                        newLoc,
                        l.toArray(EMPTY_TYPE_ANNOTATION_ARRAY),
                        getTypeAnnotations(),
                        getDecl());
            }
            return res;
        }

        @Override
        public AnnotatedType getAnnotatedOwnerType() {
            Type owner = getParameterizedType().getOwnerType();
            if (owner == null)
                return null;
            LocationInfo outerLoc = nestingForType(owner, getLocation().popAllLocations((byte)1));
            TypeAnnotation[]all = getTypeAnnotations();
            List<TypeAnnotation> l = new ArrayList<>(all.length);

            for (TypeAnnotation t : all)
                if (t.getLocationInfo().isSameLocationInfo(outerLoc))
                    l.add(t);

            return buildAnnotatedType(owner, outerLoc, l.toArray(EMPTY_TYPE_ANNOTATION_ARRAY), all, getDecl());
        }

        private ParameterizedType getParameterizedType() {
            return (ParameterizedType)getType();
        }

        @Override
        public boolean equals(Object o) {
            if (o instanceof AnnotatedParameterizedType) {
                AnnotatedParameterizedType that = (AnnotatedParameterizedType) o;
                return equalsTypeAndAnnotations(that) &&
                    Arrays.equals(getAnnotatedActualTypeArguments(), that.getAnnotatedActualTypeArguments());
            } else {
                return false;
            }
        }

        @Override
        public int hashCode() {
            return baseHashCode() ^
                Objects.hash((Object[])getAnnotatedActualTypeArguments());
        }
    }

    private static final class AnnotatedWildcardTypeImpl extends AnnotatedTypeBaseImpl implements AnnotatedWildcardType {
        private final boolean hasUpperBounds;
        AnnotatedWildcardTypeImpl(WildcardType type, LocationInfo location,
                TypeAnnotation[] actualTypeAnnotations, TypeAnnotation[] allOnSameTargetTypeAnnotations,
                AnnotatedElement decl) {
            super(type, location, actualTypeAnnotations, allOnSameTargetTypeAnnotations, decl);
            hasUpperBounds = (type.getLowerBounds().length == 0);
        }

        @Override
        public AnnotatedType[] getAnnotatedUpperBounds() {
            if (!hasUpperBounds()) {
                return new AnnotatedType[] { buildAnnotatedType(Object.class,
                        LocationInfo.BASE_LOCATION,
                        EMPTY_TYPE_ANNOTATION_ARRAY,
                        EMPTY_TYPE_ANNOTATION_ARRAY,
                        null)
                };
            }
            return getAnnotatedBounds(getWildcardType().getUpperBounds());
        }

        @Override
        public AnnotatedType[] getAnnotatedLowerBounds() {
            if (hasUpperBounds)
                return new AnnotatedType[0];
            return getAnnotatedBounds(getWildcardType().getLowerBounds());
        }

        @Override
        public AnnotatedType getAnnotatedOwnerType() {
            return null;
        }

        private AnnotatedType[] getAnnotatedBounds(Type[] bounds) {
            AnnotatedType[] res = new AnnotatedType[bounds.length];
            Arrays.fill(res, EMPTY_ANNOTATED_TYPE);
            int initialCapacity = getTypeAnnotations().length;
            for (int i = 0; i < res.length; i++) {
                LocationInfo newLoc = nestingForType(bounds[i], getLocation().pushWildcard());
                List<TypeAnnotation> l = new ArrayList<>(initialCapacity);
                for (TypeAnnotation t : getTypeAnnotations())
                    if (t.getLocationInfo().isSameLocationInfo(newLoc))
                        l.add(t);
                res[i] = buildAnnotatedType(bounds[i],
                        newLoc,
                        l.toArray(EMPTY_TYPE_ANNOTATION_ARRAY),
                        getTypeAnnotations(),
                        getDecl());
            }
            return res;
        }

        private WildcardType getWildcardType() {
            return (WildcardType)getType();
        }

        private boolean hasUpperBounds() {
            return hasUpperBounds;
        }

        @Override
        public boolean equals(Object o) {
            if (o instanceof AnnotatedWildcardType) {
                AnnotatedWildcardType that = (AnnotatedWildcardType) o;
                return equalsTypeAndAnnotations(that) &&
                    // Treats ordering as significant
                    Arrays.equals(getAnnotatedLowerBounds(), that.getAnnotatedLowerBounds()) &&
                    // Treats ordering as significant
                    Arrays.equals(getAnnotatedUpperBounds(), that.getAnnotatedUpperBounds());
            } else {
                return false;
            }
        }

        @Override
        public int hashCode() {
            return baseHashCode() ^
                Objects.hash((Object[])getAnnotatedLowerBounds()) ^
                Objects.hash((Object[])getAnnotatedUpperBounds());
        }
    }
}
