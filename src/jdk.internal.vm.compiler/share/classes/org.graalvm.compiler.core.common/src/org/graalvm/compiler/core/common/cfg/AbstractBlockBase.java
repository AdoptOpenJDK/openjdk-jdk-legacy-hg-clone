/*
 * Copyright (c) 2009, 2019, Oracle and/or its affiliates. All rights reserved.
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


package org.graalvm.compiler.core.common.cfg;

import java.util.Comparator;

public abstract class AbstractBlockBase<T extends AbstractBlockBase<T>> {

    protected int id;
    protected int domDepth;

    protected T[] predecessors;
    protected T[] successors;

    private T dominator;
    private T firstDominated;
    private T dominatedSibling;
    private int domNumber;
    private int maxChildDomNumber;

    private boolean align;
    private int linearScanNumber;

    protected AbstractBlockBase() {
        this.id = AbstractControlFlowGraph.BLOCK_ID_INITIAL;
        this.linearScanNumber = -1;
        this.domNumber = -1;
        this.maxChildDomNumber = -1;
    }

    public void setDominatorNumber(int domNumber) {
        this.domNumber = domNumber;
    }

    public void setMaxChildDomNumber(int maxChildDomNumber) {
        this.maxChildDomNumber = maxChildDomNumber;
    }

    public int getDominatorNumber() {
        return domNumber;
    }

    public int getMaxChildDominatorNumber() {
        return this.maxChildDomNumber;
    }

    public int getId() {
        return id;
    }

    public void setId(int id) {
        this.id = id;
    }

    public T[] getPredecessors() {
        return predecessors;
    }

    public void setPredecessors(T[] predecessors) {
        this.predecessors = predecessors;
    }

    public T[] getSuccessors() {
        return successors;
    }

    public void setSuccessors(T[] successors) {
        this.successors = successors;
    }

    public T getDominator() {
        return dominator;
    }

    /**
     * Returns the next dominator of this block that is either in the same loop of this block or in
     * an outer loop.
     *
     * @return the next dominator while skipping over loops
     */
    public T getDominatorSkipLoops() {
        T d = getDominator();

        if (d == null) {
            // We are at the start block and don't have a dominator.
            return null;
        }

        if (isLoopHeader()) {
            // We are moving out of current loop => just return dominator.
            assert d.getLoopDepth() == getLoopDepth() - 1;
            assert d.getLoop() != getLoop();
            return d;
        }

        while (d.getLoop() != getLoop()) {
            // We have a case where our dominator is in a different loop. Move further along
            // the domiantor tree until we hit our loop again.
            d = d.getDominator();
        }

        assert d.getLoopDepth() <= getLoopDepth();

        return d;
    }

    public void setDominator(T dominator) {
        this.dominator = dominator;
        this.domDepth = dominator.domDepth + 1;
    }

    /**
     * Level in the dominator tree starting with 0 for the start block.
     */
    public int getDominatorDepth() {
        return domDepth;
    }

    public T getFirstDominated() {
        return this.firstDominated;
    }

    public void setFirstDominated(T block) {
        this.firstDominated = block;
    }

    public T getDominatedSibling() {
        return this.dominatedSibling;
    }

    public void setDominatedSibling(T block) {
        this.dominatedSibling = block;
    }

    @Override
    public String toString() {
        return "B" + id;
    }

    public int getPredecessorCount() {
        return getPredecessors().length;
    }

    public int getSuccessorCount() {
        return getSuccessors().length;
    }

    public int getLinearScanNumber() {
        return linearScanNumber;
    }

    public void setLinearScanNumber(int linearScanNumber) {
        this.linearScanNumber = linearScanNumber;
    }

    public boolean isAligned() {
        return align;
    }

    public void setAlign(boolean align) {
        this.align = align;
    }

    public abstract boolean isExceptionEntry();

    public abstract Loop<T> getLoop();

    public abstract int getLoopDepth();

    public abstract void delete();

    public abstract boolean isLoopEnd();

    public abstract boolean isLoopHeader();

    public abstract T getPostdominator();

    public abstract double getRelativeFrequency();

    public abstract T getDominator(int distance);

    @Override
    public int hashCode() {
        return id;
    }

    public static class BlockIdComparator implements Comparator<AbstractBlockBase<?>> {
        @Override
        public int compare(AbstractBlockBase<?> o1, AbstractBlockBase<?> o2) {
            return Integer.compare(o1.getId(), o2.getId());
        }
    }

    public static final BlockIdComparator BLOCK_ID_COMPARATOR = new BlockIdComparator();
}
