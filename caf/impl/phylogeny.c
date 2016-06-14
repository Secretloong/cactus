/*
 * phlogeny.c
 *
 *  Created on: Jun 2, 2014
 *      Author: benedictpaten
 */

#include "sonLib.h"
#include "cactus.h"
#include "stPinchGraphs.h"
#include "stCactusGraphs.h"
#include "stPinchPhylogeny.h"
#include "stCaf.h"
#include "stCafPhylogeny.h"

// Struct of constant things that gets passed around. Since these are
// only set once in a run, they could be global variables, but this is
// just in case we ever need to run in parallel on sub-flowers or
// something weird.
typedef struct {
    stHash *threadStrings;
    stSet *outgroupThreads;
    Flower *flower;
    stCaf_PhylogenyParameters *params;
    stMatrix *joinCosts;
    stHash *speciesToJoinCostIndex;
    int64_t **speciesMRCAMatrix;
    stHash *eventToSpeciesNode;
    stTree *speciesStTree;
    stSet *speciesToSplitOn;
} TreeBuildingConstants;

// Gets passed to buildTreeForHomologyUnit.
typedef struct {
    HomologyUnit *homologyUnit;
    TreeBuildingConstants *constants;
    stHash *homologyUnitsToTrees;
} TreeBuildingInput;

// Gets returned from buildTreeForHomologyUnit and passed into
// addTreeToHash.
typedef struct {
    stHash *homologyUnitsToTrees;
    stTree *tree;
    HomologyUnit *homologyUnit;
    bool wasSimple;
    bool wasSingleCopy;
} TreeBuildingResult;

// Globals for collecting statistics that are later output.  Globals
// are gross, but since these don't affect the actual output of the
// program, it's probably excusable.
static int64_t numSingleDegreeSegmentsDropped = 0;
static int64_t numBasesDroppedFromSingleDegreeSegments = 0;
static int64_t totalNumberOfBlocksRecomputed = 0;
static double totalSupport = 0.0;
static int64_t numberOfSplitsMade = 0;
// These are especially bad since they are updated in a critical section.
// FIXME: (Dec 4): Remove these after the first whole-genome tests.
static int64_t numSimpleBlocksSkipped = 0;
static int64_t numSingleCopyBlocksSkipped = 0;
static FILE *gDebugFile;
static stHash *gThreadStrings;

HomologyUnit *HomologyUnit_construct(HomologyUnitType unitType, void *unit) {
    HomologyUnit *ret = st_malloc(sizeof(HomologyUnit));
    ret->unitType = unitType;
    ret->unit = unit;
    return ret;
}

void HomologyUnit_destruct(HomologyUnit *unit) {
    if (unit->unitType == CHAIN) {
        stList_destruct(unit->unit);
    }
    free(unit);
}

stHash *stCaf_getThreadStrings(Flower *flower, stPinchThreadSet *threadSet) {
    stHash *threadStrings = stHash_construct2(NULL, free);
    stPinchThreadSetIt threadIt = stPinchThreadSet_getIt(threadSet);
    stPinchThread *thread;
    while((thread = stPinchThreadSetIt_getNext(&threadIt)) != NULL) {
        Cap *cap = flower_getCap(flower, stPinchThread_getName(thread));
        assert(cap != NULL);
        Sequence *sequence = cap_getSequence(cap);
        assert(sequence != NULL);
        assert(stPinchThread_getLength(thread)-2 >= 0);
        char *string = sequence_getString(sequence, stPinchThread_getStart(thread)+1, stPinchThread_getLength(thread)-2, 1); //Gets the sequence excluding the empty positions representing the caps.
        char *paddedString = stString_print_r("N%sN", string); //Add in positions to represent the flanking bases
        stHash_insert(threadStrings, thread, paddedString);
        free(string);
    }
    gThreadStrings = threadStrings;
    return threadStrings;
}

stSet *stCaf_getOutgroupThreads(Flower *flower, stPinchThreadSet *threadSet) {
    stSet *outgroupThreads = stSet_construct();
    stPinchThreadSetIt threadIt = stPinchThreadSet_getIt(threadSet);
    stPinchThread *thread;
    while ((thread = stPinchThreadSetIt_getNext(&threadIt)) != NULL) {
        Cap *cap = flower_getCap(flower, stPinchThread_getName(thread));
        assert(cap != NULL);
        Event *event = cap_getEvent(cap);
        if(event_isOutgroup(event)) {
            stSet_insert(outgroupThreads, thread);
        }
    }
    return outgroupThreads;
}

// Gets the canonical block for a homology unit (block or chain).
// The canonical block is the block whose segments correspond to the
// leaves of the tree.
stPinchBlock *getCanonicalBlockForHomologyUnit(HomologyUnit *unit) {
    stPinchBlock *block;
    if (unit->unitType == BLOCK) {
        block = unit->unit;
    } else {
        assert(unit->unitType == CHAIN);
        block = stList_get(unit->unit, 0);
    }
    return block;
}

/*
 * Gets a list of the segments in the block that are part of outgroup threads.
 * The list contains stIntTuples, each of length 1, representing the index of a particular segment in
 * the block.
 */
static stList *getOutgroupThreads(HomologyUnit *unit, stSet *outgroupThreads) {
    stPinchBlock *block = getCanonicalBlockForHomologyUnit(unit);
    stList *outgroups = stList_construct3(0, (void (*)(void *))stIntTuple_destruct);
    stPinchBlockIt segmentIt = stPinchBlock_getSegmentIterator(block);
    stPinchSegment *segment;
    int64_t i = 0;
    while((segment = stPinchBlockIt_getNext(&segmentIt)) != NULL) {
        if(stSet_search(outgroupThreads, stPinchSegment_getThread(segment)) != NULL) {
            stList_append(outgroups, stIntTuple_construct1(i));
        }
        i++;
    }
    assert(i == stPinchBlock_getDegree(block));
    return outgroups;
}

/*
 * Splits chained blocks.
 */
stList *stCaf_splitChain(stList *chainedBlocks, stList *partitions, bool allowSingleDegreeBlocks) {
    assert(stList_length(chainedBlocks) > 0);
    // The partition is according to the order of the segments of the
    // canonical block (the first block in the chain list). So the
    // first thing we do is form a map from canonical block segment #
    // to block segment # for each block in the chain.
    stHash *blockToCanonicalBlockIndexToBlockIndex = stHash_construct2(NULL, (void (*)(void *)) stHash_destruct);
    stPinchBlock *canonicalBlock = stList_get(chainedBlocks, 0);
    stPinchBlock *lastBlock = stList_get(chainedBlocks, stList_length(chainedBlocks) - 1);
    stPinchBlockIt blockIt = stPinchBlock_getSegmentIterator(canonicalBlock);
    stPinchSegment *segment;
    int64_t canonicalBlockIndex = 0;
    while ((segment = stPinchBlockIt_getNext(&blockIt)) != NULL) {
        bool originalOrientation = stPinchSegment_getBlockOrientation(segment);
        for (;;) {
            stPinchBlock *block = stPinchSegment_getBlock(segment);
            if (block != NULL) {
                stHash *canonicalBlockIndexToBlockIndex = stHash_search(blockToCanonicalBlockIndexToBlockIndex, block);
                if (canonicalBlockIndexToBlockIndex == NULL) {
                    canonicalBlockIndexToBlockIndex = stHash_construct3((uint64_t (*)(const void *)) stIntTuple_hashKey, (int (*)(const void *, const void *)) stIntTuple_equalsFn, (void (*)(void *)) stIntTuple_destruct, (void (*)(void *)) stIntTuple_destruct);
                    stHash_insert(blockToCanonicalBlockIndexToBlockIndex, block, canonicalBlockIndexToBlockIndex);
                }
                stIntTuple *canonicalBlockIndexTuple = stIntTuple_construct1(canonicalBlockIndex);
                assert(stHash_search(canonicalBlockIndexToBlockIndex, canonicalBlockIndexTuple) == NULL);

                // Find the index of this segment within its block.
                stPinchBlockIt subBlockIt = stPinchBlock_getSegmentIterator(block);
                stPinchSegment *subSegment;
                int64_t blockIndex = 0;
                while ((subSegment = stPinchBlockIt_getNext(&subBlockIt)) != NULL) {
                    if (subSegment == segment) {
                        break;
                    }
                    blockIndex++;
                }
                assert(blockIndex < stPinchBlock_getDegree(block));

                stIntTuple *blockIndexTuple = stIntTuple_construct1(blockIndex);
                stHash_insert(canonicalBlockIndexToBlockIndex, canonicalBlockIndexTuple, blockIndexTuple);

                if (stPinchSegment_getBlock(segment) == lastBlock) {
                    break;
                }
            }
            segment = originalOrientation ? stPinchSegment_get3Prime(segment) : stPinchSegment_get5Prime(segment);
        }
        canonicalBlockIndex++;
    }

    stList *partitionedChains = stList_construct3(0, (void (*)(void *)) stList_destruct);
    for (int64_t i = 0; i < stList_length(partitions); i++) {
        stList_append(partitionedChains, stList_construct());
    }

    for (int64_t i = 0; i < stList_length(chainedBlocks); i++) {
        stPinchBlock *block = stList_get(chainedBlocks, i);
        stHash *canonicalBlockIndexToBlockIndex = stHash_search(blockToCanonicalBlockIndexToBlockIndex, block);
        assert(canonicalBlockIndexToBlockIndex != NULL);
        stList *localPartitions = stList_construct3(0, (void (*)(void *)) stList_destruct);
        for (int64_t j = 0; j < stList_length(partitions); j++) {
            stList *partition = stList_get(partitions, j);
            stList *localPartition = stList_construct();
            for (int64_t k = 0; k < stList_length(partition); k++) {
                stIntTuple *localIndex = stHash_search(canonicalBlockIndexToBlockIndex, stList_get(partition, k));
                assert(localIndex != NULL);
                stList_append(localPartition, localIndex);
            }
            stList_append(localPartitions, localPartition);
        }
        stList *locallyPartitionedBlocks = stCaf_splitBlock(block, localPartitions, allowSingleDegreeBlocks);
        for (int64_t j = 0; j < stList_length(locallyPartitionedBlocks); j++) {
            stList_append(stList_get(partitionedChains, j), stList_get(locallyPartitionedBlocks, j));
        }
        stList_destruct(locallyPartitionedBlocks);
        stList_destruct(localPartitions);
    }
    stHash_destruct(blockToCanonicalBlockIndexToBlockIndex);
    return partitionedChains;
}

/*
 * Splits the block using the given partition into a set of new blocks.
 */
stList *stCaf_splitBlock(stPinchBlock *block, stList *partitions, bool allowSingleDegreeBlocks) {
    assert(stList_length(partitions) > 0);
    stList *ret = stList_construct();
    if(stList_length(partitions) == 1) {
        //Nothing to do.
        stList_append(ret, block);
        return ret;
    }
    //Build a mapping of indices of the segments in the block to the segments
    int64_t blockDegree = stPinchBlock_getDegree(block);
    stPinchSegment **segments = st_calloc(blockDegree, sizeof(stPinchSegment *));
    bool *orientations = st_calloc(blockDegree, sizeof(bool));
    stPinchBlockIt segmentIt = stPinchBlock_getSegmentIterator(block);
    stPinchSegment *segment;
    int64_t i = 0;
    while ((segment = stPinchBlockIt_getNext(&segmentIt)) != NULL) {
        segments[i] = segment;
        assert(segments[i] != NULL);
        orientations[i++] = stPinchSegment_getBlockOrientation(segment);
    }
    assert(i == stPinchBlock_getDegree(block));
    //Destruct old block, as we build new blocks now.
    stPinchBlock_destruct(block);
    //Now build the new blocks.
    for (int64_t i=0; i<stList_length(partitions); i++) {
        stList *partition = stList_get(partitions, i);
        assert(stList_length(partition) > 0);
        int64_t k = stIntTuple_get(stList_get(partition, 0), 0);
        assert(segments[k] != NULL);
        assert(stPinchSegment_getBlock(segments[k]) == NULL);

        if (!allowSingleDegreeBlocks && stList_length(partition) == 1) {
            // We need to avoid assigning this single-degree block
            numBasesDroppedFromSingleDegreeSegments += stPinchSegment_getLength(segments[k]);
            numSingleDegreeSegmentsDropped++;
            segments[k] = NULL;
            stList_append(ret, NULL);
            continue;
        }

        block = stPinchBlock_construct3(segments[k], orientations[k]);
        assert(stPinchSegment_getBlock(segments[k]) == block);
        assert(stPinchSegment_getBlockOrientation(segments[k]) == orientations[k]);
        segments[k] = NULL; //Defensive, and used for debugging.
        for(int64_t j=1; j<stList_length(partition); j++) {
            k = stIntTuple_get(stList_get(partition, j), 0);
            assert(segments[k] != NULL);
            assert(stPinchSegment_getBlock(segments[k]) == NULL);
            stPinchBlock_pinch2(block, segments[k], orientations[k]);
            assert(stPinchSegment_getBlock(segments[k]) == block);
            assert(stPinchSegment_getBlockOrientation(segments[k]) == orientations[k]);
            segments[k] = NULL; //Defensive, and used for debugging.
        }
        stList_append(ret, block);
    }
    //Now check the segments have all been used - this is just debugging.
    for(int64_t i=0; i<blockDegree; i++) {
        assert(segments[i] == NULL);
    }
    //Cleanup
    free(segments);
    free(orientations);
    return ret;
}

/*
 * Splits a homology unit. Returns a list of homology units
 * corresponding to the partitions, in order (NULL if the unit
 * was single-degree and deleted).
 */
stList *stCaf_splitHomologyUnit(HomologyUnit *unit, stList *partitions, bool allowSingleDegreeBlocks, stHash *blocksToHomologyUnits) {
    stList *ret = stList_construct();
    if (unit->unitType == BLOCK) {
        // Remove the old homology unit mapping for this block.
        stHash_remove(blocksToHomologyUnits, unit->unit);
        stList *blocks = stCaf_splitBlock(unit->unit, partitions, allowSingleDegreeBlocks);
        for (int64_t i = 0; i < stList_length(blocks); i++) {
            stPinchBlock *block = stList_get(blocks, i);
            if (block != NULL) {
                HomologyUnit *partitionedUnit = HomologyUnit_construct(BLOCK, block);
                stHash_insert(blocksToHomologyUnits, block, partitionedUnit);
                stList_append(ret, partitionedUnit);
            } else {
                stList_append(ret, NULL);
            }
        }
    } else {
        assert(unit->unitType == CHAIN);
        // Remove any old homology unit mapping for the blocks of this chain.
        for (int64_t i = 0; i < stList_length(unit->unit); i++) {
            stHash_remove(blocksToHomologyUnits, stList_get(unit->unit, i));
        }
        stList *chains = stCaf_splitChain(unit->unit, partitions, allowSingleDegreeBlocks);
        for (int64_t i = 0; i < stList_length(chains); i++) {
            stList *chain = stList_get(chains, i);
            if (chain != NULL) {
                HomologyUnit *partitionedUnit = HomologyUnit_construct(CHAIN, chain);
                // Go through the chain and map each block to this new homology unit.
                for (int64_t j = 0; j < stList_length(chain); j++) {
                    stHash_insert(blocksToHomologyUnits, stList_get(chain, j), partitionedUnit);
                }
                stList_append(ret, partitionedUnit);
            } else {
                stList_append(ret, NULL);
            }
        }
    }
    HomologyUnit_destruct(unit);
    return ret;
}

/*
 * For logging purposes gets the total number of similarities and differences in the matrix.
 */
static void getTotalSimilarityAndDifferenceCounts(stMatrix *matrix, double *similarities, double *differences) {
    *similarities = 0.0;
    *differences = 0.0;
    for(int64_t i=0; i<stMatrix_n(matrix); i++) {
        for(int64_t j=i+1; j<stMatrix_n(matrix); j++) {
            *similarities += *stMatrix_getCell(matrix, i, j);
            *differences += *stMatrix_getCell(matrix, j, i);
        }
    }
}

// If the tree contains any zero branch lengths (i.e. there were
// negative branch lengths when neighbor-joining), fudge the branch
// lengths so that both children have non-zero branch lengths, but are
// still the same distance apart. When both children have zero branch
// lengths, give them both a small branch length. This makes
// likelihood methods usable.

// Only works on binary trees.
static void fudgeZeroBranchLengths(stTree *tree, double fudgeFactor, double smallNonZeroBranchLength) {
    assert(stTree_getChildNumber(tree) == 2 || stTree_getChildNumber(tree) == 0);
    assert(fudgeFactor < 1.0 && fudgeFactor > 0.0);
    for (int64_t i = 0; i < stTree_getChildNumber(tree); i++) {
        stTree *child = stTree_getChild(tree, i);
        fudgeZeroBranchLengths(child, fudgeFactor, smallNonZeroBranchLength);
        if (stTree_getBranchLength(child) == 0.0) {
            stTree *otherChild = stTree_getChild(tree, !i);
            if (stTree_getBranchLength(otherChild) == 0.0) {
                // Both children have zero branch lengths, set them
                // both to some very small but non-zero branch length
                // so that probabilistic methods can actually work
                stTree_setBranchLength(child, smallNonZeroBranchLength);
                stTree_setBranchLength(otherChild, smallNonZeroBranchLength);
            } else {
                // Keep the distance between the children equal, but
                // move it by fudgeFactor so that no branch length is
                // zero.
                stTree_setBranchLength(child, fudgeFactor * stTree_getBranchLength(otherChild));
                stTree_setBranchLength(otherChild, (1 - fudgeFactor) * stTree_getBranchLength(otherChild));
            }
        }
    }
}
/*
 * Get an Event pointer -> species tree node mapping.
 */
static stHash *getEventToSpeciesNode(EventTree *eventTree,
                                     stTree *speciesTree) {
    stHash *ret = stHash_construct();
    EventTree_Iterator *eventIt = eventTree_getIterator(eventTree);
    Event *event;
    while ((event = eventTree_getNext(eventIt)) != NULL) {
        char *speciesLabel = stString_print_r("%" PRIi64, event_getName(event));
        stTree *species = stTree_findChild(speciesTree, speciesLabel);
        if (species != NULL) {
            stHash_insert(ret, event, species);
        } else {
            // Every node should be in the species tree besides the root event.
            assert(event == eventTree_getRootEvent(eventTree));
        }
        free(speciesLabel);
    }
    eventTree_destructIterator(eventIt);
    return ret;
}

/*
 * Get a gene node->species node mapping from a gene tree, a species
 * tree, and the pinch block.
 */

static stHash *getLeafToSpecies(stTree *geneTree, HomologyUnit *unit,
                                Flower *flower, stHash *eventToSpeciesNode) {
    stPinchBlock *block = getCanonicalBlockForHomologyUnit(unit);
    stHash *leafToSpecies = stHash_construct();
    stPinchBlockIt blockIt = stPinchBlock_getSegmentIterator(block);
    stPinchSegment *segment;
    int64_t i = 0; // Current segment index in block.
    while((segment = stPinchBlockIt_getNext(&blockIt)) != NULL) {
        stPinchThread *thread = stPinchSegment_getThread(segment);
        Cap *cap = flower_getCap(flower, stPinchThread_getName(thread));
        Event *event = cap_getEvent(cap);
        stTree *species = stHash_search(eventToSpeciesNode, event);
        assert(species != NULL);
        stTree *gene = stPhylogeny_getLeafByIndex(geneTree, i);
        assert(gene != NULL);
        stHash_insert(leafToSpecies, gene, species);
        i++;
    }
    return leafToSpecies;
}

/*
 * Get a mapping from matrix index -> join cost index for use in
 * neighbor-joining guided by a species tree.
 */

static stHash *getMatrixIndexToJoinCostIndex(HomologyUnit *unit, Flower *flower, stHash* eventToSpeciesNode, stHash *speciesToJoinCostIndex) {
    stHash *matrixIndexToJoinCostIndex = stHash_construct3((uint64_t (*)(const void *)) stIntTuple_hashKey, (int (*)(const void *, const void *)) stIntTuple_equalsFn, (void (*)(void *)) stIntTuple_destruct, (void (*)(void *)) stIntTuple_destruct);
    stPinchBlock *block = getCanonicalBlockForHomologyUnit(unit);
    stPinchBlockIt blockIt = stPinchBlock_getSegmentIterator(block);
    stPinchSegment *segment;
    int64_t i = 0; // Current segment index in block.
    while((segment = stPinchBlockIt_getNext(&blockIt)) != NULL) {
        stPinchThread *thread = stPinchSegment_getThread(segment);
        Cap *cap = flower_getCap(flower, stPinchThread_getName(thread));
        Event *event = cap_getEvent(cap);
        stTree *species = stHash_search(eventToSpeciesNode, event);
        assert(species != NULL);

        stIntTuple *joinCostIndex = stHash_search(speciesToJoinCostIndex, species);
        assert(joinCostIndex != NULL);

        stIntTuple *matrixIndex = stIntTuple_construct1(i);
        stHash_insert(matrixIndexToJoinCostIndex, matrixIndex,
                      // Copy the join cost index so it has the same
                      // lifetime as the hash
                      stIntTuple_construct1(stIntTuple_get(joinCostIndex, 0)));
        i++;
    }
    return matrixIndexToJoinCostIndex;
}

static double scoreTree(stTree *tree, enum stCaf_ScoringMethod scoringMethod, stTree *speciesStTree, Flower *flower, stList *featureColumns, stHash *eventToSpeciesNode) {
    double ret = 0.0;
    if (scoringMethod == RECON_COST) {
        int64_t dups = 0, losses = 0;
        stPhylogeny_reconciliationCostAtMostBinary(tree, &dups, &losses);
        // Prioritize minimizing dups and use losses as tiebreakers.
        ret = -dups - 0.01*losses;
    } else if (scoringMethod == NUCLEOTIDE_LIKELIHOOD) {
        ret = stPinchPhylogeny_likelihood(tree, featureColumns);
    } else if (scoringMethod == RECON_LIKELIHOOD) {
        // FIXME: hardcoding dup-rate parameter for now
        ret = stPinchPhylogeny_reconciliationLikelihood(tree, speciesStTree, 1.0);
    } else if (scoringMethod == COMBINED_LIKELIHOOD) {
        // FIXME: hardcoding dup-rate parameter for now
        ret = stPinchPhylogeny_reconciliationLikelihood(tree, speciesStTree, 1.0);
        ret += stPinchPhylogeny_likelihood(tree, featureColumns);
    }
    return ret;
}

// Build a tree from a set of feature columns and root it according to
// the rooting method.
static stTree *buildTree(stList *featureColumns,
                         HomologyUnit *unit,
                         stCaf_PhylogenyParameters *params,
                         bool bootstrap,
                         stList *outgroups,
                         Flower *flower, stTree *speciesStTree,
                         stMatrix *joinCosts,
                         stHash *speciesToJoinCostIndex,
                         int64_t **speciesMRCAMatrix,
                         stHash *eventToSpeciesNode,
                         stMatrixDiffs *snpDiffs,
                         stMatrixDiffs *breakpointDiffs,
                         unsigned int *seed) {
    // Make substitution matrix
    stMatrix *substitutionMatrix = stPinchPhylogeny_constructMatrixFromDiffs(snpDiffs, bootstrap, seed);
    //Make breakpoint matrix
    stMatrix *breakpointMatrix = stPinchPhylogeny_constructMatrixFromDiffs(breakpointDiffs, bootstrap, seed);

    //Combine the matrices into distance matrices
    stMatrix_scale(breakpointMatrix, params->nucleotideScalingFactor, 0.0);
    stMatrix_scale(breakpointMatrix, params->breakpointScalingFactor, 0.0);
    stMatrix *combinedMatrix = stMatrix_add(substitutionMatrix, breakpointMatrix);
    stMatrix *distanceMatrix = stPinchPhylogeny_getSymmetricDistanceMatrix(combinedMatrix);

    stTree *tree = NULL;
    if (params->rootingMethod == OUTGROUP_BRANCH) {
        if (params->treeBuildingMethod == NEIGHBOR_JOINING) {
            tree = stPhylogeny_neighborJoin(distanceMatrix, outgroups);
        } else {
            assert(params->treeBuildingMethod == GUIDED_NEIGHBOR_JOINING);
            st_errAbort("Longest-outgroup-branch rooting not supported with guided neighbor joining");
        }
    } else if (params->rootingMethod == LONGEST_BRANCH) {
        if (params->treeBuildingMethod == NEIGHBOR_JOINING) {
            tree = stPhylogeny_neighborJoin(distanceMatrix, NULL);
        } else {
            assert(params->treeBuildingMethod == GUIDED_NEIGHBOR_JOINING);
            st_errAbort("Longest-branch rooting not supported with guided neighbor joining");
        }
    } else if (params->rootingMethod == BEST_RECON) {
        if (params->treeBuildingMethod == NEIGHBOR_JOINING) {
            tree = stPhylogeny_neighborJoin(distanceMatrix, NULL);
        } else {
            assert(params->treeBuildingMethod == GUIDED_NEIGHBOR_JOINING);
            // FIXME: Could move this out of the function as
            // well. It's the same for each tree generated for the
            // block.
            stHash *matrixIndexToJoinCostIndex = getMatrixIndexToJoinCostIndex(unit, flower, eventToSpeciesNode,
                                                                               speciesToJoinCostIndex);
            tree = stPhylogeny_guidedNeighborJoining(combinedMatrix, joinCosts, matrixIndexToJoinCostIndex, speciesToJoinCostIndex, speciesMRCAMatrix, speciesStTree);
            stHash_destruct(matrixIndexToJoinCostIndex);
        }
        stHash *leafToSpecies = getLeafToSpecies(tree, unit, flower,
                                                 eventToSpeciesNode);

        stTree *newTree = stPhylogeny_rootByReconciliationAtMostBinary(tree, leafToSpecies);
        stPhylogeny_addStIndexedTreeInfo(newTree);

        stPhylogenyInfo_destructOnTree(tree);
        stTree_destruct(tree);
        stHash_destruct(leafToSpecies);
        tree = newTree;
    }

    // Need to get leaf->species mapping again even when the tree is
    // already reconciled, as the nodes have changed.
    stHash *leafToSpecies = getLeafToSpecies(tree, unit, flower,
                                             eventToSpeciesNode);
    // add stReconciliationInfo.
    stPhylogeny_reconcileAtMostBinary(tree, leafToSpecies, false);
    stHash_destruct(leafToSpecies);

    // Needed for likelihood methods not to have 0/100% probabilities
    // overly often (normally, almost every other leaf has a branch
    // length of 0)
    fudgeZeroBranchLengths(tree, 0.02, 0.0001);

    stMatrix_destruct(substitutionMatrix);
    stMatrix_destruct(breakpointMatrix);
    stMatrix_destruct(combinedMatrix);
    stMatrix_destruct(distanceMatrix);
    return tree;
}

// Check if the homology unit's phylogeny is simple:
// - the unit has only one event, or
// - the unit has < 3 segments.
bool stCaf_hasSimplePhylogeny(HomologyUnit *unit, Flower *flower) {
    stPinchBlock *block = getCanonicalBlockForHomologyUnit(unit);

    if(stPinchBlock_getDegree(block) <= 2) {
        return true;
    }
    stPinchBlockIt blockIt = stPinchBlock_getSegmentIterator(block);
    stPinchSegment *segment = NULL;
    bool found2Events = 0;
    Event *currentEvent = NULL;
    while((segment = stPinchBlockIt_getNext(&blockIt)) != NULL) {
        stPinchThread *thread = stPinchSegment_getThread(segment);
        Cap *cap = flower_getCap(flower, stPinchThread_getName(thread));
        assert(cap != NULL);
        Event *event = cap_getEvent(cap);
        if(currentEvent == NULL) {
            currentEvent = event;
        } else if(currentEvent != event) {
            found2Events = 1;
        }
    }
    return !found2Events;
}

// Check if the homology unit contains as many species as segments
bool stCaf_isSingleCopy(HomologyUnit *unit, Flower *flower) {
    stPinchBlock *block = getCanonicalBlockForHomologyUnit(unit);

    stSet *seenEvents = stSet_construct();
    stPinchBlockIt blockIt = stPinchBlock_getSegmentIterator(block);
    stPinchSegment *segment = NULL;
    while((segment = stPinchBlockIt_getNext(&blockIt)) != NULL) {
        stPinchThread *thread = stPinchSegment_getThread(segment);
        Cap *cap = flower_getCap(flower, stPinchThread_getName(thread));
        assert(cap != NULL);
        Event *event = cap_getEvent(cap);
        if(stSet_search(seenEvents, event) != NULL) {
            stSet_destruct(seenEvents);
            return false;
        }
        stSet_insert(seenEvents, event);
    }
    stSet_destruct(seenEvents);
    return true;
}

// relabel a tree so it's useful for debug output
static void relabelMatrixIndexedTree(stTree *tree, stHash *matrixIndexToName) {
    for (int64_t i = 0; i < stTree_getChildNumber(tree); i++) {
        relabelMatrixIndexedTree(stTree_getChild(tree, i), matrixIndexToName);
    }
    if (stTree_getChildNumber(tree) == 0) {
        stPhylogenyInfo *info = stTree_getClientData(tree);
        assert(info != NULL);
        assert(info->index->matrixIndex != -1);
        stIntTuple *query = stIntTuple_construct1(info->index->matrixIndex);
        char *header = stHash_search(matrixIndexToName, query);
        assert(header != NULL);
        stTree_setLabel(tree, stString_print_r("%s.%"PRIi64, stString_copy(header), info->index->numBootstraps));
        stIntTuple_destruct(query);
    } else {
        stPhylogenyInfo *info = stTree_getClientData(tree);
        assert(info != NULL);
        stTree_setLabel(tree, stString_print_r("%"PRIi64, info->index->numBootstraps));
    }
}

// print debug info about blocks to the phylogeny debug dump
static void printTreeBuildingDebugInfo(Flower *flower, stPinchBlock *block, stTree *tree, FILE *outFile) {
    // First get a map from matrix indices to names
    // The format we will use for leaf names is "genome.seq|posStart-posEnd"
    stHash *matrixIndexToName = stHash_construct3((uint64_t (*)(const void *)) stIntTuple_hashKey,
                                                  (int (*)(const void *, const void *)) stIntTuple_equalsFn,
                                                  (void (*)(void *)) stIntTuple_destruct, free);
    int64_t i = 0;
    stPinchBlockIt blockIt = stPinchBlock_getSegmentIterator(block);
    stPinchSegment *segment = NULL;
    while ((segment = stPinchBlockIt_getNext(&blockIt)) != NULL) {
        stPinchThread *thread = stPinchSegment_getThread(segment);
        Cap *cap = flower_getCap(flower, stPinchThread_getName(thread));
        const char *seqHeader = sequence_getHeader(cap_getSequence(cap));
        Event *event = cap_getEvent(cap);
        const char *eventHeader = event_getHeader(event);
        char *segmentHeader = stString_print("%s.%s|%" PRIi64 "-%" PRIi64, eventHeader, seqHeader, stPinchSegment_getStart(segment), stPinchSegment_getStart(segment) + stPinchSegment_getLength(segment));
        stHash_insert(matrixIndexToName, stIntTuple_construct1(i), segmentHeader);
        i++;
    }

    // Relabel (our copy of) the best tree.
    stTree *treeCopy = stTree_clone(tree);
    relabelMatrixIndexedTree(treeCopy, matrixIndexToName);
    char *newick = stTree_getNewickTreeString(treeCopy);

    fprintf(outFile, "%s\n", newick);

    stTree_destruct(treeCopy);
    free(newick);
    stHash_destruct(matrixIndexToName);
}

static int64_t countBasesBetweenSingleDegreeBlocks(stPinchThreadSet *threadSet) {
    stPinchThreadSetIt pinchThreadIt = stPinchThreadSet_getIt(threadSet);
    stPinchThread *thread;
    int64_t numBases = 0;
    int64_t numBasesInSingleCopyBlocks = 0;
    while ((thread = stPinchThreadSetIt_getNext(&pinchThreadIt)) != NULL) {
        stPinchSegment *segment = stPinchThread_getFirst(thread);
        if (segment == NULL) {
            // No segments on this thread.
            continue;
        }
        bool wasInSingleDegreeBlock = stPinchBlock_getDegree(stPinchSegment_getBlock(segment)) == 1;
        stPinchSegment *oldSegment = NULL;
        while ((segment = stPinchSegment_get3Prime(segment)) != NULL) {
            stPinchBlock *block = stPinchSegment_getBlock(segment);
            if (block == NULL) {
                // Segment without a block.
                continue;
            }
            bool isInSingleDegreeBlock = stPinchBlock_getDegree(block) == 1;
            if (isInSingleDegreeBlock) {
                numBasesInSingleCopyBlocks += stPinchBlock_getLength(block);
            }
            int64_t numBasesBetweenSegments = 0;
            if (oldSegment != NULL) {
                numBasesBetweenSegments = stPinchSegment_getStart(segment) - (stPinchSegment_getStart(oldSegment) + stPinchSegment_getLength(oldSegment));
            }
            assert(numBasesBetweenSegments >= 0); // could be 0 if the
                                                  // blocks aren't
                                                  // identical
            if (wasInSingleDegreeBlock && isInSingleDegreeBlock) {
                numBases += numBasesBetweenSegments;
            }
            oldSegment = segment;
            wasInSingleDegreeBlock = isInSingleDegreeBlock;
        }
    }
    return numBases;
}

// Compare two bootstrap scores of split branches. Use the pointer
// value of the branches as a tiebreaker since we are using a sorted
// set and don't want to merge together all branches with the same
// support value.
int stCaf_SplitBranch_cmp(stCaf_SplitBranch *branch1,
                          stCaf_SplitBranch *branch2) {
    if (branch1->support > branch2->support) {
        return 3;
    } else if (branch1->support == branch2->support) {
        if (stTree_getBranchLength(branch1->child) > stTree_getBranchLength(branch2->child)) {
            return 2;
        } else if (stTree_getBranchLength(branch1->child) == stTree_getBranchLength(branch2->child)) {
            if (branch1->child == branch2->child) {
                return 0;
            } else if  (branch1->child > branch2->child) {
                return 1;
            } else {
                return -1;
            }
        } else {
            return -2;
        }
    } else {
        return -3;
    }
}

stCaf_SplitBranch *stCaf_SplitBranch_construct(stTree *child,
                                               HomologyUnit *unit,
                                               double support) {
    stCaf_SplitBranch *ret = calloc(1, sizeof(stCaf_SplitBranch));
    ret->support = support;
    ret->child = child;
    ret->homologyUnit = unit;
    return ret;
}

// Find new split branches from the block and add them to the sorted set.
// speciesToSplitOn is just the species that are on the path from the
// reference node to the root.
void stCaf_findSplitBranches(HomologyUnit *unit, stTree *tree,
                             stSortedSet *splitBranches,
                             stSet *speciesToSplitOn) {
    stTree *parent = stTree_getParent(tree);
    if (parent != NULL) {
        stPhylogenyInfo *parentInfo = stTree_getClientData(parent);
        assert(parentInfo != NULL);
        stReconciliationInfo *parentReconInfo = parentInfo->recon;
        assert(parentReconInfo != NULL);
        if (stSet_search(speciesToSplitOn, parentReconInfo->species)) {
            if (parentReconInfo->event == DUPLICATION) {
                // Found a split branch.
                stCaf_SplitBranch *splitBranch = stCaf_SplitBranch_construct(tree, unit, parentInfo->index->bootstrapSupport);
                stSortedSet_insert(splitBranches, splitBranch);
            }
        } else {
            // Since the reconciliation must follow the order of the
            // species tree, any child of this node cannot be
            // reconciled to the speciesToSplitOnSet.
            return;
        }
    }

    // Recurse down the tree as far as makes sense.
    for (int64_t i = 0; i < stTree_getChildNumber(tree); i++) {
        stCaf_findSplitBranches(unit, stTree_getChild(tree, i), splitBranches,
                                speciesToSplitOn);
    }
}

// Get species that are a candidate to split on if a duplication node
// is reconciled to them (i.e. everything at or above the reference
// event node).
static void getSpeciesToSplitOn(stTree *speciesTree, EventTree *eventTree,
                                const char *referenceEventHeader,
                                stSet *speciesToSplitOn) {
    if (referenceEventHeader == NULL) {
        st_errAbort("unrecoverable error: cannot find species to split on "
                    "without knowing reference event header.");
    }
    // Look through the entire species tree (which is labeled by event
    // Name as a string) and find the event whose header matches the
    // reference event.
    stTree *referenceSpecies = NULL;
    stList *bfQueue = stList_construct();
    stList_append(bfQueue, speciesTree);
    while (stList_length(bfQueue) > 0) {
        stTree *node = stList_pop(bfQueue);
        for (int64_t i = 0; i < stTree_getChildNumber(node); i++) {
            stList_append(bfQueue, stTree_getChild(node, i));
        }

        Name name;
        int ret = sscanf(stTree_getLabel(node), "%" PRIi64, &name);
        (void) ret;
        assert(ret == 1);
        Event *event = eventTree_getEvent(eventTree, name);
        const char *header = event_getHeader(event);
        if (header == NULL) {
            continue;
        }
        if (strcmp(header, referenceEventHeader) == 0) {
            referenceSpecies = node;
        }
    }
    stList_destruct(bfQueue);

    if (referenceSpecies == NULL) {
        st_errAbort("Could not find reference event header %s in species "
                    "tree.\n", referenceEventHeader);
    }

    stTree *aSpeciesToSplitOn = referenceSpecies;
    do {
        stSet_insert(speciesToSplitOn, aSpeciesToSplitOn);
        aSpeciesToSplitOn = stTree_getParent(aSpeciesToSplitOn);
    } while (aSpeciesToSplitOn != NULL);
}

static void addContextualBlocksInOneDirection(stPinchBlock *block,
                                              bool five_prime,
                                              int64_t maxBaseDistance,
                                              int64_t maxBlockDistance,
                                              bool ignoreUnalignedBases,
                                              stHash *blocksToHomologyUnits,
                                              stSet *contextualHomologyUnits) {
    stPinchBlockIt blockIt = stPinchBlock_getSegmentIterator(block);
    stPinchSegment *segment;
    while ((segment = stPinchBlockIt_getNext(&blockIt)) != NULL) {
        bool orientation = stPinchSegment_getBlockOrientation(segment);
        stPinchSegment *curSegment = (five_prime ^ orientation) ? stPinchSegment_get3Prime(segment) : stPinchSegment_get5Prime(segment);
        int64_t curBlockDistance = 0;
        int64_t curBaseDistance = stPinchSegment_getLength(segment) / 2;
        while ((curSegment != NULL) && (curBlockDistance < maxBlockDistance)
               && (curBaseDistance < maxBaseDistance)) {
            stPinchBlock *curBlock = stPinchSegment_getBlock(curSegment);
            if (curBlock != NULL) {
                HomologyUnit *unit = stHash_search(blocksToHomologyUnits, curBlock);
                stSet_insert(contextualHomologyUnits, unit);
                curBaseDistance += stPinchSegment_getLength(segment);
                curBlockDistance++;
            } else if (!ignoreUnalignedBases) {
                curBaseDistance += stPinchSegment_getLength(segment);
                curBlockDistance++;
            }
            curSegment = (five_prime ^ orientation) ? stPinchSegment_get3Prime(curSegment) : stPinchSegment_get5Prime(curSegment);
        }
    }
}

// Add any homology units close enough to the given block to be
// affected by its breakpoint information to the given set.
static void addContextualBlocksToSet(HomologyUnit *unit,
                                     int64_t maxBaseDistance,
                                     int64_t maxBlockDistance,
                                     bool ignoreUnalignedBases,
                                     stHash *blocksToHomologyUnits,
                                     stSet *contextualHomologyUnits) {
    // Go toward the 5' (relative to block orientation) end of each
    // thread adding blocks, until we reach the end or maxBaseDistance
    // or maxBlockDistance.
    stPinchBlock *block = getCanonicalBlockForHomologyUnit(unit);
    addContextualBlocksInOneDirection(block, true, maxBaseDistance, maxBlockDistance,
                                      ignoreUnalignedBases, blocksToHomologyUnits,
                                      contextualHomologyUnits);

    // Do the same for the 3' (relative to block orientation) side.
    if (unit->unitType == CHAIN) {
        block = stList_get(unit->unit, stList_length(unit->unit) - 1);
    }
    addContextualBlocksInOneDirection(block, false, maxBaseDistance, maxBlockDistance,
                                      ignoreUnalignedBases, blocksToHomologyUnits,
                                      contextualHomologyUnits);
}

// Remove any split branches that appear in this tree from the
// set of split branches.
void stCaf_removeSplitBranches(HomologyUnit *unit, stTree *tree,
                               stSet *speciesToSplitOn,
                               stSortedSet *splitBranches) {
    if (tree == NULL) {
        return;
    }
    stSortedSet *splitBranchesToDelete = stSortedSet_construct3((int (*)(const void *, const void *)) stCaf_SplitBranch_cmp, free);
    stCaf_findSplitBranches(unit, tree, splitBranchesToDelete,
                            speciesToSplitOn);
    // Could set splitBranches = splitBranches \ splitBranchesToDelete.
    // But this is probably faster.
    stSortedSetIterator *splitBranchToDeleteIt = stSortedSet_getIterator(splitBranchesToDelete);
    stCaf_SplitBranch *splitBranchToDelete;
    while ((splitBranchToDelete = stSortedSet_getNext(splitBranchToDeleteIt)) != NULL) {
        stSortedSet_remove(splitBranches, splitBranchToDelete);
    }
    stSortedSet_destructIterator(splitBranchToDeleteIt);
    stSortedSet_destruct(splitBranchesToDelete);
}

// TODO: tmp for debug
static double getAvgSupportValue(stSortedSet *splitBranches) {
    double totalSupport = 0.0;
    stSortedSetIterator *splitBranchIt = stSortedSet_getIterator(splitBranches);
    stCaf_SplitBranch *splitBranch;
    while ((splitBranch = stSortedSet_getNext(splitBranchIt)) != NULL) {
        totalSupport += splitBranch->support;
    }
    stSortedSet_destructIterator(splitBranchIt);
    if (stSortedSet_size(splitBranches) == 0) {
        return 0.0;
    }
    return totalSupport/stSortedSet_size(splitBranches);
}

// Small wrapper function to tell the pool to build, reconcile, and
// bootstrap a tree for a homology unit.
static void pushHomologyUnitToPool(HomologyUnit *unit,
                                   TreeBuildingConstants *constants,
                                   stHash *homologyUnitsToTrees,
                                   stThreadPool *threadPool) {
    TreeBuildingInput *input = st_malloc(sizeof(TreeBuildingInput));
    input->constants = constants;
    input->homologyUnit = unit;
    input->homologyUnitsToTrees = homologyUnitsToTrees;
    stThreadPool_push(threadPool, input);
}

// Gets run as a worker in a thread.
static TreeBuildingResult *buildTreeForHomologyUnit(TreeBuildingInput *input) {
    HomologyUnit *unit = input->homologyUnit;
    stCaf_PhylogenyParameters *params = input->constants->params;

    TreeBuildingResult *ret = st_calloc(1, sizeof(TreeBuildingResult));
    ret->homologyUnitsToTrees = input->homologyUnitsToTrees;
    ret->homologyUnit = unit;

    if (stCaf_hasSimplePhylogeny(unit, input->constants->flower)) {
        // No point trying to build a phylogeny for certain blocks.
        free(input);
        ret->wasSimple = true;
        return ret;
    }
    if (stCaf_isSingleCopy(unit, input->constants->flower)
        && params->skipSingleCopyBlocks) {
        ret->wasSingleCopy = true;
        free(input);
        return ret;
    }

    // Get the feature blocks.
    stList *featureBlocks;

    if (unit->unitType == BLOCK) {
        featureBlocks = stFeatureBlock_getContextualFeatureBlocks(
            unit->unit, params->maxBaseDistance,
            params->maxBlockDistance,
            params->ignoreUnalignedBases,
            params->onlyIncludeCompleteFeatureBlocks,
            input->constants->threadStrings);
    } else {
        assert(unit->unitType == CHAIN);
        featureBlocks = stFeatureBlock_getContextualFeatureBlocksForChainedBlocks(
            unit->unit, params->maxBaseDistance,
            params->maxBlockDistance,
            params->ignoreUnalignedBases,
            params->onlyIncludeCompleteFeatureBlocks,
            input->constants->threadStrings);
    }

    // Make feature columns
    stList *featureColumns = stFeatureColumn_getFeatureColumns(featureBlocks);

    // Get the outgroup threads
    stList *outgroups = getOutgroupThreads(unit,
                                           input->constants->outgroupThreads);


    // Get the degree (= number of segments in the block/chain).
    int64_t degree = stPinchBlock_getDegree(getCanonicalBlockForHomologyUnit(unit));

    // Get the matrix diffs.
    stMatrixDiffs *snpDiffs = stPinchPhylogeny_getMatrixDiffsFromSubstitutions(featureColumns, degree, NULL);
    stMatrixDiffs *breakpointDiffs = stPinchPhylogeny_getMatrixDiffsFromBreakpoints(featureColumns, degree, NULL);

    // rand() has a global lock on it! Better to contest it once and
    // use that as a seed than to contest it several thousand times
    // per tree...
    unsigned int mySeed = rand();

    // Build the canonical tree.
    stTree *canonicalTree = buildTree(featureColumns, unit, params,
                                      0, outgroups, input->constants->flower,
                                      input->constants->speciesStTree,
                                      input->constants->joinCosts,
                                      input->constants->speciesToJoinCostIndex,
                                      input->constants->speciesMRCAMatrix,
                                      input->constants->eventToSpeciesNode,
                                      snpDiffs, breakpointDiffs, &mySeed);

    // Sample the rest of the trees.
    stList *trees = stList_construct();
    stList_append(trees, canonicalTree);
    for (int64_t i = 0; i < params->numTrees - 1; i++) {
        stTree *tree = buildTree(featureColumns, unit, params,
                                 1, outgroups, input->constants->flower,
                                 input->constants->speciesStTree,
                                 input->constants->joinCosts,
                                 input->constants->speciesToJoinCostIndex,
                                 input->constants->speciesMRCAMatrix,
                                 input->constants->eventToSpeciesNode,
                                 snpDiffs, breakpointDiffs, &mySeed);
        stList_append(trees, tree);
    }

    stMatrixDiffs_destruct(snpDiffs);
    stMatrixDiffs_destruct(breakpointDiffs);

    // Get the best-scoring tree.
    double maxScore = -INFINITY;
    stTree *bestTree = NULL;
    for (int64_t i = 0; i < stList_length(trees); i++) {
        stTree *tree = stList_get(trees, i);
        double score = scoreTree(tree, params->scoringMethod,
                                 input->constants->speciesStTree,
                                 input->constants->flower, featureColumns,
                                 input->constants->eventToSpeciesNode);
        if (score > maxScore) {
            maxScore = score;
            bestTree = tree;
        }
    }

    if(bestTree == NULL) {
        // Can happen if/when the nucleotide likelihood score
        // is used and a block is all N's. Just use the
        // canonical NJ tree in that case.
        bestTree = canonicalTree;
    }

    assert(bestTree != NULL);

    // Update the bootstrap support for each branch.
    stTree *bootstrapped = stPhylogeny_scoreReconciliationFromBootstraps(bestTree, trees);

    // Cleanup
    for (int64_t i = 0; i < stList_length(trees); i++) {
        stTree *tree = stList_get(trees, i);
        stPhylogenyInfo_destructOnTree(tree);
        stTree_destruct(tree);
    }
    stList_destruct(trees);
    stList_destruct(featureColumns);
    stList_destruct(featureBlocks);
    stList_destruct(outgroups);
    free(input);

    ret->tree = bootstrapped;

    return ret;
}

// Gets run as a "finisher" in the thread pool, so it's run in series
// and we don't have to lock the hash.
static void addTreeToHash(TreeBuildingResult *result) {
    if (stHash_search(result->homologyUnitsToTrees, result->homologyUnit)) {
        stHash_remove(result->homologyUnitsToTrees, result->homologyUnit);
    }
    if (result->tree != NULL) {
        stHash_insert(result->homologyUnitsToTrees, result->homologyUnit, result->tree);
    } else {
        if (result->wasSimple) {
            numSimpleBlocksSkipped++;
        } else if (result->wasSingleCopy) {
            numSingleCopyBlocksSkipped++;
        }
    }
    free(result); // Sucks to have to do this in a critical section,
                  // but shouldn't matter too much.
}

// When splitting an existing tree by removing the edge corresponding
// to a split branch, we need to relabel the two resulting trees'
// leaves' matrix indices so that they refer to their positions in the
// two new blocks, not their positions in the old block.
static void relabelTreeIndices(stTree *tree, stList *leafSet) {
    for (int64_t i = 0; i < stList_length(leafSet); i++) {
        int64_t oldIndex = stIntTuple_get(stList_get(leafSet, i), 0);
        stTree *leaf = stPhylogeny_getLeafByIndex(tree, oldIndex);
        assert(leaf != NULL);
        // This *looks* like it could run into problems since the leaf
        // set is not sorted -- i.e. we could rename something to "0"
        // and then look for an old index of "0" later on -- but since
        // stPhylogeny_getLeafByIndex only looks at the
        // stIndexedTreeInfo, not the labels, this is safe.
        char *label = stString_print_r("%" PRIi64, i);
        stTree_setLabel(leaf, label);
        free(label);
    }

    stPhylogeny_addStIndexedTreeInfo(tree);
}

void splitOnSplitBranch(stCaf_SplitBranch *splitBranch,
                        stSortedSet *splitBranches,
                        TreeBuildingConstants *constants,
                        stHash *blocksToHomologyUnits,
                        stHash *homologyUnitsToTrees,
                        stSet *homologyUnitsToUpdate) {
    HomologyUnit *unit = splitBranch->homologyUnit;

    // Check if this block is marked to be updated later on. If so,
    // remove it from the set, as it will be invalidated.
    stSet_remove(homologyUnitsToUpdate, unit);

    // Do the partition on the block.
    stList *partition = stList_construct();

    // Create a leaf set with all leaves below this split branch.
    stList *leafSet = stList_construct3(0, (void (*)(void *))stIntTuple_destruct);
    stList *bfQueue = stList_construct();
    stList_append(bfQueue, splitBranch->child);
    while (stList_length(bfQueue) != 0) {
        stTree *node = stList_pop(bfQueue);
        for (int64_t i = 0; i < stTree_getChildNumber(node); i++) {
            stList_append(bfQueue, stTree_getChild(node, i));
        }

        if (stTree_getChildNumber(node) == 0) {
            stPhylogenyInfo *info = stTree_getClientData(node);
            assert(info->index->matrixIndex != -1);
            stList_append(leafSet, stIntTuple_construct1(info->index->matrixIndex));
        }
    }
    stList_append(partition, leafSet);

    // Create a leaf set with all leaves that aren't below the
    // split branch.
    leafSet = stList_construct3(0, (void (*)(void *))stIntTuple_destruct);
    stTree *root = splitBranch->child;
    while (stTree_getParent(root) != NULL) {
        root = stTree_getParent(root);
    }

    /* if (gDebugFile != NULL) { */
    /*     fprintf(gDebugFile, "split:\n"); */
    /*     printTreeBuildingDebugInfo(constants->flower, block, root, gDebugFile); */
    /* } */

    stList_append(bfQueue, root);
    while (stList_length(bfQueue) != 0) {
        stTree *node = stList_pop(bfQueue);
        if (node != splitBranch->child) {
            for (int64_t i = 0; i < stTree_getChildNumber(node); i++) {
                stList_append(bfQueue, stTree_getChild(node, i));
            }

            if (stTree_getChildNumber(node) == 0) {
                stPhylogenyInfo *info = stTree_getClientData(node);
                assert(info->index->matrixIndex != -1);
                stList_append(leafSet, stIntTuple_construct1(info->index->matrixIndex));
            }
        }
    }
    stList_append(partition, leafSet);

    // Remove all split branch entries for this block tree.
    stCaf_removeSplitBranches(unit, root, constants->speciesToSplitOn,
                              splitBranches);

    assert(stHash_search(homologyUnitsToTrees, unit) == root);
    stHash_remove(homologyUnitsToTrees, unit);

    // Actually perform the split according to the partition.
    stList *partitionedUnits = stCaf_splitHomologyUnit(unit, partition,
                                                       constants->params->keepSingleDegreeBlocks,
                                                       blocksToHomologyUnits);

    HomologyUnit *unitBelowBranch = stList_get(partitionedUnits, 0);
    HomologyUnit *unitNotBelowBranch = stList_get(partitionedUnits, 1);

    // Get a new tree for each unit using the existing tree.
    stTree *treeNotBelowBranch = root;
    stTree *treeBelowBranch = splitBranch->child;
    // Need to remove the extra node (which will have degree 2 once
    // the below-branch subtree is removed).
    stTree *extraNode = stTree_getParent(treeBelowBranch);
    assert(extraNode != NULL);
    stTree_setParent(treeBelowBranch, NULL);
    assert(stTree_getChildNumber(extraNode) == 1);
    if (extraNode == treeNotBelowBranch) {
        // If the extra node happens to be the tree root, it still
        // needs to be deleted, but we need to make sure not to lose
        // the reference to the supertree.
        treeNotBelowBranch = stTree_getChild(extraNode, 0);
    }
    stTree_setParent(stTree_getChild(extraNode, 0), stTree_getParent(extraNode));
    stTree_setParent(extraNode, NULL);
    stTree_destruct(extraNode);

    // Now relabel the two split trees so that they correspond to the
    // segments of their new units.
    relabelTreeIndices(treeBelowBranch, stList_get(partition, 0));
    relabelTreeIndices(treeNotBelowBranch, stList_get(partition, 1));

    // Add the split branches from these new units back into the set.
    stCaf_findSplitBranches(unitBelowBranch, treeBelowBranch,
                            splitBranches, constants->speciesToSplitOn);
    stCaf_findSplitBranches(unitNotBelowBranch, treeNotBelowBranch,
                            splitBranches, constants->speciesToSplitOn);
    stHash_insert(homologyUnitsToTrees, unitBelowBranch, treeBelowBranch);
    stHash_insert(homologyUnitsToTrees, unitNotBelowBranch, treeNotBelowBranch);

    /* if (gDebugFile != NULL) { */
    /*     fprintf(gDebugFile, "below:\n"); */
    /*     if (blockBelowBranch != NULL) { */
    /*         printTreeBuildingDebugInfo(constants->flower, blockBelowBranch, treeBelowBranch, gDebugFile); */
    /*     } */
    /*     fprintf(gDebugFile, "above:\n"); */
    /*     if (blockNotBelowBranch != NULL) { */
    /*         printTreeBuildingDebugInfo(constants->flower, blockNotBelowBranch, treeNotBelowBranch, gDebugFile); */
    /*     } */
    /* } */

    // Finally, add any units that could have been affected by the
    // breakpoint information to the homologyUnitsToUpdate set.
    if (unitBelowBranch != NULL) {
        stSet_insert(homologyUnitsToUpdate, unitBelowBranch);
        addContextualBlocksToSet(unitBelowBranch,
                                 constants->params->maxBaseDistance,
                                 constants->params->maxBlockDistance,
                                 constants->params->ignoreUnalignedBases,
                                 blocksToHomologyUnits,
                                 homologyUnitsToUpdate);
    }
    if (unitNotBelowBranch != NULL) {
        stSet_insert(homologyUnitsToUpdate, unitNotBelowBranch);
        addContextualBlocksToSet(unitNotBelowBranch,
                                 constants->params->maxBaseDistance,
                                 constants->params->maxBlockDistance,
                                 constants->params->ignoreUnalignedBases,
                                 blocksToHomologyUnits,
                                 homologyUnitsToUpdate);
    }

    stList_destruct(partitionedUnits);
}

static void destructTree(stTree *tree) {
    stPhylogenyInfo_destructOnTree(tree);
    stTree_destruct(tree);
}

// Update the trees that belong to each block in the homologyUnitsToUpdate
// set. Invalidates all pointers to the old trees or their split
// branches, and adds the new split branches to the set.
static void recomputeAffectedTrees(stSet *homologyUnitsToUpdate,
                                   TreeBuildingConstants *constants,
                                   stThreadPool *treeBuildingPool,
                                   stHash *homologyUnitsToTrees,
                                   stSortedSet *splitBranches) {
    stSetIterator *homologyUnitsToUpdateIt = stSet_getIterator(homologyUnitsToUpdate);
    stList *unitsToPush = stList_construct();
    HomologyUnit *unitToUpdate;
    while ((unitToUpdate = stSet_getNext(homologyUnitsToUpdateIt)) != NULL) {
        totalNumberOfBlocksRecomputed++;
        stTree *oldTree = stHash_search(homologyUnitsToTrees, unitToUpdate);
        stCaf_removeSplitBranches(unitToUpdate, oldTree,
                                  constants->speciesToSplitOn, splitBranches);
        stList_append(unitsToPush, unitToUpdate);
    }
    stSet_destructIterator(homologyUnitsToUpdateIt);

    for (int64_t i = 0; i < stList_length(unitsToPush); i++) {
        pushHomologyUnitToPool(stList_get(unitsToPush, i), constants,
                               homologyUnitsToTrees, treeBuildingPool);
    }

    // Wait for the trees to be done.
    stThreadPool_wait(treeBuildingPool);
    homologyUnitsToUpdateIt = stSet_getIterator(homologyUnitsToUpdate);
    while ((unitToUpdate = stSet_getNext(homologyUnitsToUpdateIt)) != NULL) {
        stTree *tree = stHash_search(homologyUnitsToTrees, unitToUpdate);
        if (tree != NULL) {
            stCaf_findSplitBranches(unitToUpdate, tree,
                                    splitBranches, constants->speciesToSplitOn);
        }
    }
    stList_destruct(unitsToPush);
    stSet_destructIterator(homologyUnitsToUpdateIt);
}

// Split on a single branch and update the blocks affected immediately.
static void splitUsingSingleBranch(stCaf_SplitBranch *splitBranch,
                                   stSortedSet *splitBranches,
                                   TreeBuildingConstants *constants,
                                   stHash *blocksToHomologyUnits,
                                   stThreadPool *treeBuildingPool,
                                   stHash *homologyUnitsToTrees) {
    totalSupport += splitBranch->support;
    stSet *homologyUnitsToUpdate = stSet_construct();
    splitOnSplitBranch(splitBranch, splitBranches, constants, blocksToHomologyUnits,
                       homologyUnitsToTrees, homologyUnitsToUpdate);
    recomputeAffectedTrees(homologyUnitsToUpdate, constants, treeBuildingPool,
                           homologyUnitsToTrees, splitBranches);
    stSet_destruct(homologyUnitsToUpdate);
    numberOfSplitsMade++;
}

// Split all highly confident branches at once, then update the
// affected blocks all at once. Asssuming there is more than one
// confident split branch, we save a ton of time.
static void splitUsingHighlyConfidentBranches(stCaf_SplitBranch *splitBranch,
                                              stSortedSet *splitBranches,
                                              TreeBuildingConstants *constants,
                                              stHash *blocksToHomologyUnits,
                                              stThreadPool *treeBuildingPool,
                                              stHash *homologyUnitsToTrees) {
    stSet *homologyUnitsToUpdate = stSet_construct();
    while (splitBranch != NULL && splitBranch->support > constants->params->doSplitsWithSupportHigherThanThisAllAtOnce) {
        totalSupport += splitBranch->support;
        splitOnSplitBranch(splitBranch, splitBranches, constants, blocksToHomologyUnits,
                           homologyUnitsToTrees, homologyUnitsToUpdate);
        // Get the next split branch.
        splitBranch = stSortedSet_getLast(splitBranches);
        numberOfSplitsMade++;
    }

    recomputeAffectedTrees(homologyUnitsToUpdate, constants, treeBuildingPool,
                           homologyUnitsToTrees, splitBranches);
    stSet_destruct(homologyUnitsToUpdate);
}

static int64_t indexOf(stList *list, void *item) {
    for (int64_t i = 0; i < stList_length(list); i++) {
        if (stList_get(list, i) == item) {
            return i;
        }
    }
    return -1;
}

// Return a list with correct start and end from the cyclical chain ordering.
stList *stCaf_getCorrectChainOrder(stList *blocks) {
    stPinchSegment *segment = stPinchBlock_getFirst(stList_get(blocks, 0));
    // Orientation of the first segment, which determines what direction we travel.
    bool orientation = stPinchSegment_getBlockOrientation(segment);
    int64_t curBlockIndex = 0;
    int64_t newStartIndex = 0;

    while (1) {
        segment = orientation ? stPinchSegment_get3Prime(segment) : stPinchSegment_get5Prime(segment );
        if (segment == NULL || curBlockIndex == stList_length(blocks) - 1) {
            newStartIndex = (curBlockIndex + 1) % stList_length(blocks);
            break;
        }
        stPinchBlock *block = stPinchSegment_getBlock(segment);
        if (block == NULL) {
            continue;
        }
        // This may end up being very slow as it searches linearly through the list.
        int64_t index = indexOf(blocks, block);

        if (index == -1) {
            break;
        } else if (index != curBlockIndex + 1) {
            newStartIndex = index;
            break;
        }
        curBlockIndex = index;
    }
    if (newStartIndex != 0) {
        stList *newBlocks = stList_construct2(stList_length(blocks));
        for (int64_t i = 0; i < stList_length(blocks); i++) {
            stList_set(newBlocks, i, stList_get(blocks, (i + newStartIndex) % stList_length(blocks)));
        }
        stList_destruct(blocks);
        blocks = newBlocks;
    }

    return blocks;
}

// Get the initial homology units from the graph.
static stSet *getHomologyUnits(Flower *flower, stPinchThreadSet *threadSet, stHash *blocksToHomologyUnits, HomologyUnitType type) {
    stSet *homologyUnits = stSet_construct2((void (*)(void *)) HomologyUnit_destruct);
    if (type == BLOCK) {
        stPinchThreadSetBlockIt blockIt = stPinchThreadSet_getBlockIt(threadSet);
        stPinchBlock *block;
        while ((block = stPinchThreadSetBlockIt_getNext(&blockIt)) != NULL) {
            HomologyUnit *unit = HomologyUnit_construct(BLOCK, block);
            stSet_insert(homologyUnits, unit);
            stHash_insert(blocksToHomologyUnits, block, unit);
        }
    } else {
        assert(type == CHAIN);
        stCactusNode *startCactusNode;
        stList *deadEndComponent;
        stCactusGraph *cactusGraph = stCaf_getCactusGraphForThreadSet(flower, threadSet, &startCactusNode, &deadEndComponent, 0, 0,
                                                                      0.0, true, 100000);
        stCactusGraphNodeIt *nodeIt = stCactusGraphNodeIterator_construct(cactusGraph);
        stCactusNode *cactusNode;
        while ((cactusNode = stCactusGraphNodeIterator_getNext(nodeIt)) != NULL) {
            stCactusNodeEdgeEndIt cactusEdgeEndIt = stCactusNode_getEdgeEndIt(cactusNode);
            stCactusEdgeEnd *cactusEdgeEnd;
            while ((cactusEdgeEnd = stCactusNodeEdgeEndIt_getNext(&cactusEdgeEndIt)) != NULL) {
                if (stCactusEdgeEnd_isChainEnd(cactusEdgeEnd) && stCactusEdgeEnd_getLinkOrientation(cactusEdgeEnd)) {
                    // Found a canonical chain end, traverse the chain
                    // and create a homology unit from it.
                    stCactusEdgeEnd *chainEnd = cactusEdgeEnd;
                    stCactusEdgeEnd *curEnd = cactusEdgeEnd;
                    stList *blocks = stList_construct();
                    HomologyUnit *unit = HomologyUnit_construct(CHAIN, blocks);
                    stSet_insert(homologyUnits, unit);
                    do {
                        stPinchEnd *pinchEnd = stCactusEdgeEnd_getObject(curEnd);
                        stPinchBlock *block = stPinchEnd_getBlock(pinchEnd);
                        // We will visit almost every block twice, so
                        // avoid a second insertion causing an
                        // expensive hash removal.
                        if (stHash_search(blocksToHomologyUnits, block) == NULL) {
                            stHash_insert(blocksToHomologyUnits, block, unit);
                            stList_append(blocks, block);
                        } else {
                            assert(stHash_search(blocksToHomologyUnits, block) == unit);
                        }
                        if (stCactusEdgeEnd_getLinkOrientation(curEnd)) {
                            curEnd = stCactusEdgeEnd_getLink(curEnd);
                        } else {
                            curEnd = stCactusEdgeEnd_getOtherEdgeEnd(curEnd);
                        }
                    } while (curEnd != chainEnd);

                    // The chain may not have the "correct" start and
                    // end--arrange it so that it is increasing
                    // position relative to the block orientation.
                    unit->unit = stCaf_getCorrectChainOrder(blocks);
                    assert(!stList_contains(unit->unit, NULL));
                }
            }
        }
        stCactusGraphNodeIterator_destruct(nodeIt);
        stCactusGraph_destruct(cactusGraph);
    }
    return homologyUnits;
}

void stCaf_buildTreesToRemoveAncientHomologies(stPinchThreadSet *threadSet,
                                               stHash *threadStrings,
                                               stSet *outgroupThreads,
                                               Flower *flower,
                                               stCaf_PhylogenyParameters *params,
                                               FILE *debugFile,
                                               const char *referenceEventHeader) {
    // Functions we aren't using right now but should stick around anyway.
    (void) getTotalSimilarityAndDifferenceCounts;

    stPinchThreadSetBlockIt blockIt = stPinchThreadSet_getBlockIt(threadSet);
    stPinchBlock *block;

    //Hash in which we store a map of homology units to their trees
    stHash *homologyUnitsToTrees = stHash_construct2(NULL, (void (*)(void *))destructTree);

    //Get species tree as an stTree
    EventTree *eventTree = flower_getEventTree(flower);
    stTree *speciesStTree = eventTree_getStTree(eventTree);

    // Get info for reconciliation.
    stHash *eventToSpeciesNode = getEventToSpeciesNode(eventTree, speciesStTree);

    // Get info for guided neighbor-joining
    stHash *speciesToJoinCostIndex = stHash_construct2(NULL, (void (*)(void *)) stIntTuple_destruct);
    stMatrix *joinCosts = stPhylogeny_computeJoinCosts(
        speciesStTree, speciesToJoinCostIndex,
        params->costPerDupPerBase * 2 * params->maxBaseDistance,
        params->costPerLossPerBase * 2 * params->maxBaseDistance);
    int64_t **speciesMRCAMatrix = stPhylogeny_getMRCAMatrix(speciesStTree, speciesToJoinCostIndex);

    stSortedSet *splitBranches = stSortedSet_construct3((int (*)(const void *, const void *)) stCaf_SplitBranch_cmp, free);
    stSet *speciesToSplitOn = stSet_construct();
    getSpeciesToSplitOn(speciesStTree, eventTree, referenceEventHeader,
                        speciesToSplitOn);

    printf("Chose events in the species tree \"%s\" to split on:", eventTree_makeNewickString(eventTree));
    stSetIterator *speciesToSplitOnIt = stSet_getIterator(speciesToSplitOn);
    stTree *speciesNodeToSplitOn;
    while ((speciesNodeToSplitOn = stSet_getNext(speciesToSplitOnIt)) != NULL) {
        Name name;
        sscanf(stTree_getLabel(speciesNodeToSplitOn), "%" PRIi64, &name);
        Event *event = eventTree_getEvent(eventTree, name);
        printf(" %s", event_getHeader(event));
    }
    printf("\n");
    stSet_destructIterator(speciesToSplitOnIt);

    stThreadPool *treeBuildingPool = stThreadPool_construct(
        params->numTreeBuildingThreads,
        (void *(*)(void *)) buildTreeForHomologyUnit,
        (void (*)(void *)) addTreeToHash);
    TreeBuildingConstants constants;
    constants.threadStrings = threadStrings;
    constants.outgroupThreads = outgroupThreads;
    constants.flower = flower;
    constants.params = params;
    constants.joinCosts = joinCosts;
    constants.speciesToJoinCostIndex = speciesToJoinCostIndex;
    constants.speciesMRCAMatrix = speciesMRCAMatrix;
    constants.eventToSpeciesNode = eventToSpeciesNode;
    constants.speciesStTree = speciesStTree;
    constants.speciesToSplitOn = speciesToSplitOn;

    gDebugFile = debugFile;

    // This hash stores a mapping (kept up-to-date after every split)
    // from blocks to the homology units that contain them. This is
    // necessary because the cactus structure won't be updated for
    // splits, so we need to keep a special mapping from block to
    // (potentially split) chain.
    stHash *blocksToHomologyUnits = stHash_construct();

    stSet *homologyUnits = getHomologyUnits(flower, threadSet, blocksToHomologyUnits, CHAIN);

    // The loop to build a tree for each homology unit
    stSetIterator *homologyUnitIt = stSet_getIterator(homologyUnits);
    HomologyUnit *unit;
    while ((unit = stSet_getNext(homologyUnitIt)) != NULL) {
        pushHomologyUnitToPool(unit, &constants, homologyUnitsToTrees, treeBuildingPool);
    }
    stSet_destructIterator(homologyUnitIt);

    // We need the trees to be done before we can continue.
    stThreadPool_wait(treeBuildingPool);

    if (debugFile != NULL) {
        blockIt = stPinchThreadSet_getBlockIt(threadSet);
        while ((block = stPinchThreadSetBlockIt_getNext(&blockIt)) != NULL) {
            stTree *tree = stHash_search(homologyUnitsToTrees, block);
            if (tree != NULL) {
                printTreeBuildingDebugInfo(flower, block, tree, debugFile);
            }
        }
    }

    fprintf(stdout, "First tree-building round done. Skipped %" PRIi64
            " simple blocks (those with 1 event or with < 3 segments, and %"
            PRIi64 " single-copy blocks (those with (# events) = (# segments))."
            "\n", numSimpleBlocksSkipped, numSingleCopyBlocksSkipped);

    // All the blocks have their trees computed. Find the split
    // branches in those trees.
    stHashIterator *homologyUnitsToTreesIt = stHash_getIterator(homologyUnitsToTrees);
    while ((unit = stHash_getNext(homologyUnitsToTreesIt)) != NULL) {
        stTree *tree = stHash_search(homologyUnitsToTrees, unit);
        assert(tree != NULL);
        stCaf_findSplitBranches(unit, tree, splitBranches, speciesToSplitOn);
    }

    fprintf(stdout, "Before partitioning, there were %" PRIi64 " bases lost in between single-degree blocks\n", countBasesBetweenSingleDegreeBlocks(threadSet));
    fprintf(stdout, "Found %" PRIi64 " split branches initially in %" PRIi64
            " blocks (%" PRIi64 " of which have trees), with an "
            "average support value of %lf\n", stSortedSet_size(splitBranches),
            stPinchThreadSet_getTotalBlockNumber(threadSet),
            stHash_size(homologyUnitsToTrees),
            getAvgSupportValue(splitBranches));

    // Now walk through the split branches, doing the most confident
    // splits first, and updating the blocks whose breakpoint
    // information is modified.
    stCaf_SplitBranch *splitBranch = stSortedSet_getLast(splitBranches);
    while (splitBranch != NULL) {
        if (splitBranch->support > params->doSplitsWithSupportHigherThanThisAllAtOnce) {
            // This split branch is well-supported, and likely there
            // are others that are well-supported as well. These are
            // unlikely to be improved by more accurate breakpoint
            // information, so we split them all at once and then only
            // recompute the affected block trees in one go.
            splitUsingHighlyConfidentBranches(splitBranch, splitBranches,
                                              &constants, blocksToHomologyUnits,
                                              treeBuildingPool, homologyUnitsToTrees);
        } else {
            // None of the split branches left in the set have good
            // support. We start to split one at a time, hoping that
            // the iterative increase in the quality of the breakpoint
            // information will encourage splits that leave us with a
            // sensible graph.
            splitUsingSingleBranch(splitBranch, splitBranches,
                                   &constants, blocksToHomologyUnits,
                                   treeBuildingPool, homologyUnitsToTrees);
        }
        splitBranch = stSortedSet_getLast(splitBranches);
    }

    if (debugFile != NULL) {
        fprintf(debugFile, "post melting:\n");
        blockIt = stPinchThreadSet_getBlockIt(threadSet);
        while ((block = stPinchThreadSetBlockIt_getNext(&blockIt)) != NULL) {
            fprintf(debugFile, "[");
            stPinchBlockIt segIt = stPinchBlock_getSegmentIterator(block);
            int64_t i = 0;
            stPinchSegment *segment;
            while ((segment = stPinchBlockIt_getNext(&segIt))) {
                stPinchThread *thread = stPinchSegment_getThread(segment);
                Cap *cap = flower_getCap(flower, stPinchThread_getName(thread));
                const char *seqHeader = sequence_getHeader(cap_getSequence(cap));
                Event *event = cap_getEvent(cap);
                const char *eventHeader = event_getHeader(event);
                char *segmentHeader = stString_print("%s.%s|%" PRIi64 "-%" PRIi64, eventHeader, seqHeader, stPinchSegment_getStart(segment), stPinchSegment_getStart(segment) + stPinchSegment_getLength(segment));
                if (i != 0) {
                    fprintf(debugFile, ",");
                }
                fprintf(debugFile, "\"%s\"", segmentHeader);
                free(segmentHeader);
                i++;
            }
            fprintf(debugFile, "]\n");
        }
    }

    st_logDebug("Finished partitioning the blocks\n");
    fprintf(stdout, "There were %" PRIi64 " splits made overall in the end.\n",
            numberOfSplitsMade);
    fprintf(stdout, "The split branches that we actually used had an average "
            "support of %lf.\n",
            numberOfSplitsMade != 0 ? totalSupport/numberOfSplitsMade : 0.0);
    fprintf(stdout, "We recomputed the trees for an average of %" PRIi64
            " blocks after every split, and the pinch graph had a total of "
            "%" PRIi64 " blocks.\n",
            numberOfSplitsMade != 0 ? totalNumberOfBlocksRecomputed/numberOfSplitsMade : 0,
            stPinchThreadSet_getTotalBlockNumber(threadSet));
    fprintf(stdout, "After partitioning, there were %" PRIi64 " bases lost in between single-degree blocks\n", countBasesBetweenSingleDegreeBlocks(threadSet));
    fprintf(stdout, "We stopped %" PRIi64 " single-degree segments from becoming"
            " blocks (avg %lf per block) for a total of %" PRIi64 " bases\n",
            numSingleDegreeSegmentsDropped,
            ((float)numSingleDegreeSegmentsDropped)/stPinchThreadSet_getTotalBlockNumber(threadSet),
            numBasesDroppedFromSingleDegreeSegments);

    //Cleanup
    for (int64_t i = 0; i < stTree_getNumNodes(speciesStTree); i++) {
        free(speciesMRCAMatrix[i]);
    }
    free(speciesMRCAMatrix);
    stTree_destruct(speciesStTree);
    stThreadPool_destruct(treeBuildingPool);
    stHash_destruct(homologyUnitsToTrees);
    stHash_destruct(blocksToHomologyUnits);
}
