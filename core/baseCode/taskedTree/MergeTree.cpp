/*
 * file: MergeTree.cpp
 * description: MergeTree processing package.
 * author: Gueunet Charles
 * date: Dec 2016
 */

#include "MergeTree.h"

// -----------
// CONSTRUCT
// -----------
// {

MergeTree::MergeTree(Params *const params, Triangulation *mesh, Scalars *const scalars,
                     TreeType type)
    : params_(params), mesh_(mesh), scalars_(scalars)
{
   treeData_.treeType = type;

   treeData_.superArcs   = nullptr;
   treeData_.nodes       = nullptr;
   treeData_.roots       = nullptr;
   treeData_.leaves      = nullptr;
   treeData_.vert2tree   = nullptr;
   treeData_.ufs         = nullptr;
   treeData_.propagation = nullptr;
   treeData_.valences    = nullptr;
   treeData_.openedNodes = nullptr;

#ifdef withStatsHeight
   treeData_.arcDepth     = nullptr;
   treeData_.arcPotential = nullptr;
#endif

#ifdef withStatsTime
   treeData_.arcStart = nullptr;
   treeData_.arcEnd   = nullptr;
   treeData_.arcOrig  = nullptr;
   treeData_.arcTasks = nullptr;
#endif
}

MergeTree::~MergeTree()
{
   // all is automatically destroyed in treedata
}

// }
// -------
// Process
// -------
// {

void MergeTree::build(const bool ct)
{
    string treeString;
   // --------------------------
   // Comparator init (template)
   // --------------------------
   if (treeData_.treeType == TreeType::Join) {
      treeString = "JT";
      comp_.vertLower = [this](idVertex a, idVertex b) -> bool {
         return this->scalars_->isLower(a, b);
      };
      comp_.vertHigher = [this](idVertex a, idVertex b) -> bool {
         return this->scalars_->isHigher(a, b);
      };
   } else {
      treeString = "ST";
      comp_.vertLower = [this](idVertex a, idVertex b) -> bool {
         return this->scalars_->isHigher(a, b);
      };
      comp_.vertHigher = [this](idVertex a, idVertex b) -> bool {
         return this->scalars_->isLower(a, b);
      };
   }

   // ----------------------------
   // Build Merge treeString using tasks
   // ----------------------------
   DebugTimer precomputeTime;
   int alreadyDone = precompute();
   printTime(precomputeTime, "3 precompute " + treeString, scalars_->size, 2 + alreadyDone);

   DebugTimer buildTime;
   leaves();
   printTime(buildTime, "4 leaves "+treeString, scalars_->size);

   DebugTimer bbTime;
   idVertex bbSize = trunk();
   printTime(bbTime, "5 trunk "+treeString, bbSize);

   // ------------
   // Segmentation
   // ------------
   if (ct) {
      DebugTimer segmTime;
      buildSegmentation();
      printTime(segmTime, "6 segmentation " + treeString, scalars_->size);
   }

   // -----
   // Stats
   // -----
   stats();
}

// extrema

int MergeTree::precompute()
{
   int ret = 0;
   // if not already computed by CT
   if(getNumberOfNodes() == 0){
      const auto nbScalars = scalars_->size;
      const auto chunkSize = getChunkSize();
      const auto chunkNb   = getChunkCount();

      // --------------------------------
      // Extrema extract and launch tasks
      // --------------------------------
      for (idVertex chunkId = 0; chunkId < chunkNb; ++chunkId) {
#pragma omp task firstprivate(chunkId)
         {
            const idVertex lowerBound = chunkId * chunkSize;
            const idVertex upperBound = min(nbScalars, (chunkId + 1) * chunkSize);
            for (idVertex v = lowerBound; v < upperBound; ++v) {
               const auto &neighNumb = mesh_->getVertexNeighborNumber(v);
               valence     val       = 0;

               for (valence n = 0; n < neighNumb; ++n) {
                  idVertex neigh;
                  mesh_->getVertexNeighbor(v, n, neigh);
                  comp_.vertLower(neigh, v) && ++val;
               }

               (*treeData_.valences)[v] = val;

               if (!val) {
                  makeNode(v);
               }
            }
         }
      }

#pragma omp taskwait
   } else {
       ret = 1;
   }

   // fill leaves
   const auto& nbLeaves = treeData_.nodes->size();
   treeData_.leaves->resize(nbLeaves);
   std::iota(treeData_.leaves->begin(), treeData_.leaves->end(), 0);

   if (debugLevel_ >= 3) {
      cout << "nb leaves " << nbLeaves << endl;
   }

   // -------
   // Reserve Arcs
   // -------
   treeData_.superArcs->reserve(nbLeaves * 2 + 1);
#ifdef withStatsTime
   createVector<float>(treeData_.arcStart);
   createVector<float>(treeData_.arcEnd);
   createVector<idVertex>(treeData_.arcOrig);
   createVector<idNode>(treeData_.arcTasks);
   treeData_.arcStart->resize(nbLeaves*2 +1,0);
   treeData_.arcEnd->resize(nbLeaves*2 +1,0);
   treeData_.arcOrig->resize(nbLeaves*2 +1,0);
   treeData_.arcTasks->resize(nbLeaves*2 +1,0);
#endif

   return ret;
}

// skeleton

DebugTimer _launchGlobalTime;

void MergeTree::leaves()
{
   _launchGlobalTime.reStart();

   const auto &nbLeaves = treeData_.leaves->size();

   // elevation: backbone only
   if (nbLeaves == 1) {
      const idVertex v            = (*treeData_.nodes)[0].getVertexId();
      (*treeData_.openedNodes)[v] = 1;
      (*treeData_.ufs)[v]         = new AtomicUF(v);
      return;
   }

   treeData_.activeTasks = nbLeaves;

   // Need testing, simulate priority
   // best with gcc
   auto comp = [this](const idNode a, const idNode b) {
      return this->comp_.vertLower(this->getNode(a)->getVertexId(),
                                   this->getNode(b)->getVertexId());
   };
   sort(treeData_.leaves->begin(), treeData_.leaves->end(), comp);

   for (idNode n = 0; n < nbLeaves; ++n)
   {
      const idNode l = (*treeData_.leaves)[n];
      int          v = getNode(l)->getVertexId();
      // for each node: get vert, create uf and lauch
      (*treeData_.ufs)[v] = new AtomicUF(v);

#pragma omp task untied
      processTask(v, 0, n);
   }

#pragma omp taskwait
}

#ifdef withStatsHeight
idNode _maxDepth = 1;
#endif

void MergeTree::processTask(const idVertex startVert, const idNode d, const idVertex orig)
{

#ifdef withStatsHeight
# pragma omp critical
   {
      if (d > _maxDepth) {
         _maxDepth = d;
      }
   }
#endif

   // ------------------------
   // current task id / propag
   // ------------------------
   UF startUF = (*treeData_.ufs)[startVert]->find();
   // get or recover states
   CurrentState *currentState;
   if (startUF->getNbStates()) {
      currentState = startUF->getFirstState();
   } else {
      currentState = new CurrentState(startVert, comp_.vertHigher);
      startUF->addState(currentState);
   }

   currentState->addNewVertex(startVert);

   // avoid duplicate processing of startVert
   bool seenFirst = false;

   // -----------
   // ARC OPENING
   // -----------
   idNode     startNode  = getCorrespondingNodeId(startVert);
   idSuperArc currentArc = openSuperArc(startNode);
   startUF->addArcToClose(currentArc);
#ifdef withStatsTime
   (*treeData_.arcStart)[currentArc] = _launchGlobalTime.getElapsedTime();
   (*treeData_.arcOrig)[currentArc] = orig;
   (*treeData_.arcTasks)[currentArc] = treeData_.activeTasks;
#endif

   // ----------------
   // TASK PROPAGATION
   // ----------------
   while (!currentState->empty()) {
      // -----------
      // Next vertex
      // -----------
      idVertex currentVert = currentState->getNextMinVertex();

      // ignore duplicate
      if (!isCorrespondingNull(currentVert) && !isCorrespondingNode(currentVert)) {
         continue;
      } else {
         // first node can be duplicate, avoid duplicate process
         if (currentVert == startVert) {
            if (!seenFirst) {
               seenFirst = true;
            } else {
               continue;
            }
         }
      }

      // -------------------------------------
      // Saddle & Last detection + propagation
      // -------------------------------------
      bool isSaddle, isLast;
      tie(isSaddle, isLast) = propage(*currentState, startUF);

      // regular propagation
#pragma omp atomic write seq_cst
      (*treeData_.ufs)[currentVert] = startUF;

      // -----------
      // Saddle case
      // -----------
      if (isSaddle) {

# ifdef withStatsTime
         (*treeData_.arcEnd)[currentArc] = _launchGlobalTime.getElapsedTime();
# endif
         // need a node on this vertex
         (*treeData_.openedNodes)[currentVert] = 1;

         // ---------------------------
         // If last close all and merge
         // ---------------------------
         if (isLast) {
            // last task detection
            idNode remainingTasks;
#pragma omp atomic read seq_cst
            remainingTasks = treeData_.activeTasks;
            if (remainingTasks == 1) {
                // only backbone remaining
                return;
            }

             // finish works here
            closeAndMergeOnSaddle(currentVert);

            // made a node on this vertex
#pragma omp atomic write seq_cst
            (*treeData_.openedNodes)[currentVert] = 0;

            // recursively continue
#pragma omp taskyield
            processTask(currentVert, d+1, orig);
         } else {
            // Active tasks / threads
#pragma omp atomic update seq_cst
            treeData_.activeTasks--;
         }

         // stop at saddle
         return;
      }

      if (currentVert != startVert) {
         updateCorrespondingArc(currentVert, currentArc);
      }
      getSuperArc(currentArc)->setLastVisited(currentVert);

   }  // end wile propagation

// ----------
// close root
// ----------
   const idVertex closeVert      = getSuperArc(currentArc)->getLastVisited();
   bool           existCloseNode = isCorrespondingNode(closeVert);
   idNode closeNode = (existCloseNode) ? getCorrespondingNodeId(closeVert) : makeNode(closeVert);
   closeSuperArc(currentArc, closeNode);
   getSuperArc(currentArc)->decrNbSeen();
   idNode rootPos              = treeData_.roots->getNext();
   (*treeData_.roots)[rootPos] = closeNode;

#ifdef withStatsTime
   (*treeData_.arcEnd)[currentArc] = _launchGlobalTime.getElapsedTime();
#endif
}

tuple<bool, bool> MergeTree::propage(CurrentState &currentState, UF curUF)
{
   bool        becameSaddle = false, isLast = false;
   const auto &nbNeigh = mesh_->getVertexNeighborNumber(currentState.vertex);
   valence decr = 0;

   // once for all
   auto* curUFF = curUF->find();

   // propagation / is saddle
   for (valence n = 0; n < nbNeigh; ++n) {
      idVertex neigh;
      mesh_->getVertexNeighbor(currentState.vertex, n, neigh);

      if (comp_.vertLower(neigh, currentState.vertex)) {
         UF neighUF = (*treeData_.ufs)[neigh];

         // is saddle
         if (!neighUF || neighUF->find() != curUFF) {
            becameSaddle = true;
         } else if (neighUF) {
             ++decr;
         }

      } else {
         if (!(*treeData_.propagation)[neigh] ||
             (*treeData_.propagation)[neigh]->find() != curUFF) {
            currentState.addNewVertex(neigh);
            (*treeData_.propagation)[neigh] = curUFF;
         }
      }
   }

   // is last
   valence  oldVal;
#pragma omp atomic capture
   {
      oldVal = (*treeData_.valences)[currentState.vertex];
      (*treeData_.valences)[currentState.vertex] -= decr;
   }
   if (oldVal == decr) {
      isLast = true;
   }

   return make_tuple(becameSaddle, isLast);
}

void MergeTree::closeAndMergeOnSaddle(idVertex saddleVert)
{
   idNode closeNode = makeNode(saddleVert);

   // Union of the UF coming here (merge propagation and closing arcs)
   const auto &nbNeigh = mesh_->getVertexNeighborNumber(saddleVert);
   for (valence n = 0; n < nbNeigh; ++n) {
      idVertex neigh;
      mesh_->getVertexNeighbor(saddleVert, n, neigh);

      if (comp_.vertLower(neigh, saddleVert)) {
         if ((*treeData_.ufs)[neigh]->find() != (*treeData_.ufs)[saddleVert]->find()) {

            (*treeData_.ufs)[saddleVert] =
                AtomicUF::makeUnion((*treeData_.ufs)[saddleVert], (*treeData_.ufs)[neigh]);
         }
      }
   }

   // close arcs on this node
   closeArcsUF(closeNode, (*treeData_.ufs)[saddleVert]);

   (*treeData_.ufs)[saddleVert]->find()->mergeStates();
   (*treeData_.ufs)[saddleVert]->find()->setExtrema(saddleVert);
}

void MergeTree::closeOnBackBone(idVertex saddleVert)
{
   idNode closeNode = makeNode(saddleVert);

   // Union of the UF coming here (merge propagation and closing arcs)
   const auto &nbNeigh = mesh_->getVertexNeighborNumber(saddleVert);
   for (valence n = 0; n < nbNeigh; ++n) {
      idVertex neigh;
      mesh_->getVertexNeighbor(saddleVert, n, neigh);

      if (comp_.vertLower(neigh, saddleVert)) {
         if ((*treeData_.ufs)[neigh] &&
             (*treeData_.ufs)[neigh]->find() != (*treeData_.ufs)[saddleVert]->find()) {

            (*treeData_.ufs)[saddleVert] =
                AtomicUF::makeUnion((*treeData_.ufs)[saddleVert], (*treeData_.ufs)[neigh]);
         }
      }
   }

   // close arcs on this node
   closeArcsUF(closeNode, (*treeData_.ufs)[saddleVert]);
}

void MergeTree::closeArcsUF(idNode closeNode, UF uf)
{
   for (const auto &sa : uf->find()->getOpenedArcs()) {
      closeSuperArc(sa, closeNode);
   }
   uf->find()->clearOpenedArcs();
}

idVertex MergeTree::trunk()
{
   DebugTimer bbTimer;

   vector<idVertex>pendingVerts;
   const auto &nbScalars = scalars_->size;

   // -----
   //pendingVerts
   // -----
  pendingVerts.reserve(max(10, nbScalars / 500));
   for (idVertex v = 0; v < nbScalars; ++v) {
       if((*treeData_.openedNodes)[v]){
          pendingVerts.emplace_back(v);
       }
   }
   sort(pendingVerts.begin(), pendingVerts.end(), comp_.vertLower);
   for (const idVertex v : pendingVerts) {
      closeOnBackBone(v);
   }

   // ----
   // Arcs
   // ----

   const auto &nbNodes =pendingVerts.size();
   for (idNode n = 1; n < nbNodes; ++n) {
      idSuperArc na =
          makeSuperArc(getCorrespondingNodeId(pendingVerts[n - 1]), getCorrespondingNodeId(pendingVerts[n]));
      getSuperArc(na)->setLastVisited(pendingVerts[n]);
   }

   if (!nbNodes) {
      return 0;
   }
   const idSuperArc lastArc = openSuperArc(getCorrespondingNodeId(pendingVerts[nbNodes - 1]));

   // debug close Root
   const idNode rootNode = makeNode((*scalars_->sortedVertices)[(isJT())?scalars_->size -1:0]);
   closeSuperArc(lastArc, rootNode);
   getSuperArc(lastArc)->setLastVisited(getNode(rootNode)->getVertexId());

   printTime(bbTimer, "Backbone seq.", -1, 3);
   bbTimer.reStart();

// ------------
// Segmentation
// ------------
#ifdef withStatsRatio
   idVertex duplicateSeen = 0;
#endif
   // bounds
   idVertex begin, stop;
   tie(begin, stop)         = getBoundsFromVerts(pendingVerts);
   const auto sizeBackBone  = abs(stop - begin);
   const int nbTasksThreads = 40;
   const auto chunkSize     = getChunkSize(sizeBackBone, nbTasksThreads);
   const auto chunkNb       = getChunkCount(sizeBackBone, nbTasksThreads);

   // si pas efficace vecteur de la taille de node ici a la place de acc
   idNode   lastVertInRange = 0;
   idVertex acc             = 0;
   if (isJT()) {
      for (idVertex chunkId = 0; chunkId < chunkNb; ++chunkId) {
#pragma omp task firstprivate(chunkId)
         {
            const idVertex lowerBound = begin + chunkId * chunkSize;
            const idVertex upperBound = min(stop, (begin + (chunkId + 1) * chunkSize));
            for (idVertex v = lowerBound; v < upperBound; ++v) {
               assignChunkTrunk(pendingVerts, lastVertInRange, acc, v);
            }
            // force increment last arc
            const idNode     baseNode = getCorrespondingNodeId(pendingVerts[lastVertInRange]);
            const idSuperArc upArc    = getNode(baseNode)->getUpSuperArcId(0);
            getSuperArc(upArc)->atomicIncVisited(acc);
         }
      }
   } else {
      for (idVertex chunkId = chunkNb - 1; chunkId >= 0; --chunkId) {
#pragma omp task firstprivate(chunkId)
         {
            const idVertex upperBound = begin - chunkId * chunkSize;
            const idVertex lowerBound = max(stop, begin - (chunkId + 1) * chunkSize);
            for (idVertex v = upperBound; v > lowerBound; --v) {
               assignChunkTrunk(pendingVerts, lastVertInRange, acc, v);
            }
            // force increment last arc
            const idNode     baseNode = getCorrespondingNodeId(pendingVerts[lastVertInRange]);
            const idSuperArc upArc    = getNode(baseNode)->getUpSuperArcId(0);
            getSuperArc(upArc)->atomicIncVisited(acc);
         }
      }
   }
#pragma omp taskwait

   printTime(bbTimer, "Backbone para.", -1, 3);

#ifdef withStatsRatio
   cout << "duplicate : " << duplicateSeen << " / " << (stop-begin) << endl;
#endif

   // ---------------------
   // Root (close last arc)
   // ---------------------
   // if several CC still the backbone is only in one.
   // But the root may not be the max node of the whole dataset
   return sizeBackBone;
}

void MergeTree::assignChunkTrunk(const vector<idVertex> &pendingVerts, idNode &lastVertInRange,
                                 idVertex &acc, const idVertex v)
{
   const idVertex s = (*scalars_->sortedVertices)[v];
   if (isCorrespondingNull(s)) {
      const idNode oldVertInRange = lastVertInRange;
      lastVertInRange             = getVertInRange(pendingVerts, s, lastVertInRange);
      const idSuperArc thisArc    = upArcFromVert(pendingVerts[lastVertInRange]);
      updateCorrespondingArc(s, thisArc);
      if (oldVertInRange == lastVertInRange) {
         ++acc;
      } else {
         // accumulated to have only one atomic update when needed
         const idSuperArc oldArc = upArcFromVert(pendingVerts[oldVertInRange]);
         getSuperArc(oldArc)->atomicIncVisited(acc);
         acc = 1;
      }
   }
#ifdef withStatsRatio
   else {
#pragma omp atomic update
            ++duplicateSeen;
         }
#endif
}

// stats

void MergeTree::stats()
{

#ifdef withStatsHeight
   cout << "arcs " << treeData_.superArcs->size() << endl;
   cout << "depth " << _maxDepth << endl;

   initPtrVector<idSuperArc>(treeData_.arcDepth);
   treeData_.arcDepth->resize(treeData_.superArcs->size(), nullSuperArc);

   const auto nbleaves = treeData_.leaves->size();
   vector<idNode> heights(nbleaves, 0);

   for (idNode l = 0; l < nbleaves; ++l) {
      heights[l] = height(l);
   }

   float avg = 0, var = 0;

   // Max
   const auto& itMax = max_element(heights.cbegin(), heights.cend());
   idNode heightval = *itMax;

   cout << "height " << heightval << endl;

   // Avg
   for (const auto& h : heights) {
      avg += h;
   }
   avg /= nbleaves;

   cout << "avg    " << avg << endl;

   // Var
   for (const auto& h : heights) {
       var += powf((h-avg), 2);
   }
   var /= nbleaves;

   float stddev = sqrtf(var);

   cout << "var    " << var << endl;
   cout << "stddev " << stddev << endl;

   // Segm size
   initPtrVector<idSuperArc>(treeData_.arcPotential);
   treeData_.arcPotential->resize(treeData_.superArcs->size(), nullVertex);

   createArcPotential();

#endif
}

idNode MergeTree::height(const idNode& node, const idNode h)
{
#ifdef withStatsHeight
    if(getNode(node)->getNumberOfUpSuperArcs()){

       const idSuperArc &upArc = getNode(node)->getUpSuperArcId(0);
       if((*treeData_.arcDepth)[upArc] == nullSuperArc || (*treeData_.arcDepth)[upArc] < h){
           (*treeData_.arcDepth)[upArc] = h;
       }

       return height(getParent(node), h + 1);
    }
#endif
    return h;
}

void MergeTree::createArcPotential()
{
    for (const idNode& r : (*treeData_.roots)) {
       arcPotential(r);
    }
}

void MergeTree::arcPotential(const idNode parentId, const idVertex pot)
{
#ifdef withStatsHeight
   auto *parentNode = getNode(parentId);
   const auto& nbChilds = parentNode->getNumberOfDownSuperArcs();
   for (idSuperArc c = 0; c < nbChilds; c++) {
      const idSuperArc curChild = parentNode->getDownSuperArcId(c);
      const idVertex curSegm = getSuperArc(curChild)->getRegion().count();

      (*treeData_.arcPotential)[curChild] = pot + curSegm;

      arcPotential(getSuperArc(curChild)->getDownNodeId(), pot + curSegm);
   }
#endif
}

// segmentation

void MergeTree::buildSegmentation()
{

   const idSuperArc nbArcs = treeData_.superArcs->size();

   // ------------
   // Make reserve
   // ------------
   // SuperArc i correspond to segment i,
   // one arc correspond to one segment
   vector<idVertex> sizes(nbArcs);

   // get the size of each segment
   const idSuperArc arcChunkSize = getChunkSize(nbArcs);
   const idSuperArc arcChunkNb   = getChunkCount(nbArcs);
   for(idSuperArc arcChunkId = 0; arcChunkId < arcChunkNb; ++arcChunkId){
       // WHY shared(sizes) is needed ??
#pragma omp task firstprivate(arcChunkId) shared(sizes)
       {
           const idSuperArc lowerBound = arcChunkId*arcChunkSize;
           const idSuperArc upperBound = min(nbArcs, (arcChunkId+1)*arcChunkSize );
           for (idSuperArc a = lowerBound; a < upperBound; ++a) {
              sizes[a] = max(0, (*treeData_.superArcs)[a].getNbVertSeen() - 1);
           }
       }
   }
#pragma omp taskwait

   // change segments size using the created vector
   treeData_.segments_.resize(sizes);

   DebugTimer segmentsSet;
   // -----------------------------
   // Fill segments using vert2tree
   // -----------------------------
   // current status of the segmentation of this arc
   vector<idVertex> posSegm(nbArcs, 0);

   // Segments are connex region of geometrie forming
   // the segmentation (sorted in ascending order)
   const idVertex nbVert = scalars_->size;
   const idVertex chunkSize = getChunkSize();
   const idVertex chunkNb   = getChunkCount();
   for (idVertex chunkId = 0; chunkId < chunkNb; ++chunkId)
   {
#pragma omp task firstprivate(chunkId) shared(posSegm)
       {
          const idVertex lowerBound = chunkId * chunkSize;
          const idVertex upperBound = min(nbVert, (chunkId+1)*chunkSize);
          for (idVertex i = lowerBound; i < upperBound; ++i) {
             const auto &vert = (*scalars_->sortedVertices)[i];
             if (isCorrespondingArc(vert)) {
                idSuperArc sa = getCorrespondingSuperArcId(vert);
                idVertex   vertToAdd;
#pragma omp atomic capture
                vertToAdd = posSegm[sa]++;

                treeData_.segments_[sa][vertToAdd] = vert;
             }
          }
       }
   }
#pragma omp taskwait

   printTime(segmentsSet, "segm. set verts", -1, 3);

   DebugTimer segmentsSortTime;
   treeData_.segments_.sortAll(scalars_);
   printTime(segmentsSortTime, "segm. sort verts", -1, 3);

   // ----------------------
   // Update SuperArc region
   // ----------------------
   // ST have a segmentation wich is in the reverse-order of its build
   // ST have a segmentation sorted in ascending order as JT
   for(idSuperArc arcChunkId = 0; arcChunkId < arcChunkNb; ++arcChunkId){
#pragma omp task firstprivate(arcChunkId)
      {
         const idSuperArc lowerBound = arcChunkId * arcChunkSize;
         const idSuperArc upperBound = min(nbArcs, (arcChunkId + 1) * arcChunkSize);
         for (idSuperArc a = lowerBound; a < upperBound; ++a) {
            // avoid empty region
            if (treeData_.segments_[a].size()) {
               (*treeData_.superArcs)[a].concat(treeData_.segments_[a].begin(),
                                                treeData_.segments_[a].end());
            }
         }

      }
   }
#pragma omp taskwait
}

// }
// ---------------------------
// Arcs and node manipulations
// ---------------------------
// {
// SuperArcs
// .......................{
idSuperArc MergeTree::openSuperArc(idNode downNodeId)
{
#ifndef withKamikaze
   if (downNodeId < 0 || (size_t)downNodeId >= getNumberOfNodes()) {
      cout << "[Merge Tree] openSuperArc on a inexisting node !" << endl;
      return -2;
   }
#endif

   idSuperArc newSuperArcId = treeData_.superArcs->getNext();
   (*treeData_.superArcs)[newSuperArcId].setDownNodeId(downNodeId);
   (*treeData_.nodes)[downNodeId].addUpSuperArcId(newSuperArcId);

   return newSuperArcId;
}

idSuperArc MergeTree::makeSuperArc(idNode downNodeId, idNode upNodeId)

{
   idSuperArc newSuperArcId = treeData_.superArcs->getNext();
   (*treeData_.superArcs)[newSuperArcId].setDownNodeId(downNodeId);
   (*treeData_.superArcs)[newSuperArcId].setUpNodeId(upNodeId);

   (*treeData_.nodes)[downNodeId].addUpSuperArcId(newSuperArcId);
   (*treeData_.nodes)[upNodeId].addDownSuperArcId(newSuperArcId);

   return newSuperArcId;
}

void MergeTree::closeSuperArc(idSuperArc superArcId, idNode upNodeId)
{
#ifndef withKamikaze

   if (superArcId < 0 || (size_t)superArcId >= getNumberOfSuperArcs()) {
      cout << "[Merge Tree] closeSuperArc on a inexisting arc !" << endl;
      return;
   }

   if (upNodeId < 0 || (size_t)upNodeId >= getNumberOfNodes()) {
      cout << "[Merge Tree] closeOpenedArc on a inexisting node !" << endl;
      return;
   }

#endif
   (*treeData_.superArcs)[superArcId].setUpNodeId(upNodeId);
   (*treeData_.nodes)[upNodeId].addDownSuperArcId(superArcId);
}

// state

void MergeTree::mergeArc(idSuperArc sa, idSuperArc recept, const bool changeConnectivity)
{
   (*treeData_.superArcs)[sa].merge(recept);

   if (changeConnectivity) {
      (*treeData_.nodes)[(*treeData_.superArcs)[sa].getUpNodeId()].removeDownSuperArc(sa);
      (*treeData_.nodes)[(*treeData_.superArcs)[sa].getDownNodeId()].removeUpSuperArc(sa);
   }
}

//   }
// nodes
// .....................{

vector<idNode> MergeTree::sortedNodes(const bool para)
{
   vector<idNode> sortedNodes(treeData_.nodes->size());
   std::iota(sortedNodes.begin(), sortedNodes.end(), 0);

   auto indirect_sort = [&](const idNode a, const idNode b) {
      return comp_.vertLower(getNode(a)->getVertexId(), getNode(b)->getVertexId());
   };

   if (para) {
#ifdef __clang__
      std::sort(sortedNodes.begin(), sortedNodes.end(), indirect_sort);
#else
      __gnu_parallel::sort(sortedNodes.begin(), sortedNodes.end(), indirect_sort);
#endif
   } else {
#pragma omp single
      {
         std::sort(sortedNodes.begin(), sortedNodes.end(), indirect_sort);
      }
   }

   return sortedNodes;
}

// add

idNode MergeTree::makeNode(idVertex vertexId, idVertex term)
{
#ifndef withKamikaze
   if (vertexId < 0 || vertexId >= scalars_->size) {
      cout << "[Merge Tree] make node, wrong vertex :" << vertexId << " on " << scalars_->size
           << endl;
      return -1;
   }
#endif

   if (isCorrespondingNode(vertexId)) {
      return getCorrespondingNodeId(vertexId);
   }

   idNode newNodeId = treeData_.nodes->getNext();
   (*treeData_.nodes)[newNodeId].setVertexId(vertexId);
   (*treeData_.nodes)[newNodeId].setTerminaison(term);
   updateCorrespondingNode(vertexId, newNodeId);

   return newNodeId;
}

idNode MergeTree::makeNode(const Node *const n, idVertex term)
{
   return makeNode(n->getVertexId());
}

// Normal insert : existing arc stay below inserted (JT example)
//  *   - <- upNodeId
//  | \ |   <- newSA
//  |   * <- newNodeId
//  |   |   <- currentSA
//  - - -
idSuperArc MergeTree::insertNode(Node *node, const bool segm)
{
   // already present
   if (isCorrespondingNode(node->getVertexId())) {
      Node *myNode = vertex2Node(node->getVertexId());
      // If it has been hidden / replaced we need to re-make it
      SuperArc * sa                 = getSuperArc(myNode->getUpSuperArcId(0));
      idSuperArc correspondingArcId = (sa->getReplacantArcId() == nullSuperArc)
          ? myNode->getUpSuperArcId(0)
          : sa->getReplacantArcId();
      updateCorrespondingArc(myNode->getVertexId(), correspondingArcId);
   }

   idNode     upNodeId, newNodeId;
   idSuperArc currentSA, newSA;
   idVertex   origin;

   // Create new node
   currentSA = getCorrespondingSuperArcId(node->getVertexId());
   upNodeId  = (*treeData_.superArcs)[currentSA].getUpNodeId();
   origin    = (*treeData_.nodes)[(*treeData_.superArcs)[currentSA].getDownNodeId()].getOrigin();
   newNodeId = makeNode(node, origin);

   // Connectivity
   // Insert only node inside the partition : created arc don t cross
   newSA = makeSuperArc(newNodeId, upNodeId);

   (*treeData_.superArcs)[currentSA].setUpNodeId(newNodeId);
   (*treeData_.nodes)[upNodeId].removeDownSuperArc(currentSA);
   (*treeData_.nodes)[newNodeId].addDownSuperArcId(currentSA);

   // cut the vertex list at the node position and
   // give each arc its part.
   if (segm) {
      if (treeData_.treeType == TreeType::Split) {
         (*treeData_.superArcs)[newSA].concat(
             get<1>((*treeData_.superArcs)[currentSA].splitBack(node->getVertexId(), scalars_)));
      } else {
         (*treeData_.superArcs)[newSA].concat(
             get<1>((*treeData_.superArcs)[currentSA].splitFront(node->getVertexId(), scalars_)));
      }
   }

   return newSA;
}

// traverse

Node *MergeTree::getDownNode(const SuperArc *a)
{
   return &((*treeData_.nodes)[a->getDownNodeId()]);
}

Node *MergeTree::getUpNode(const SuperArc *a)
{
   return &((*treeData_.nodes)[a->getUpNodeId()]);
}

// remove

void MergeTree::delNode(idNode node)
{
   Node *mainNode = getNode(node);

   if (mainNode->getNumberOfUpSuperArcs() == 0) {
// -----------------
// Root: No Superarc
// -----------------

#ifndef withKamikaze
      if (mainNode->getNumberOfDownSuperArcs() != 1) {
         // Root with several childs: impossible /\ .
         cout << endl << "[MergeTree]:delNode won't delete ";
         cout << mainNode->getVertexId() << " (root) with ";
         cout << static_cast<unsigned>(mainNode->getNumberOfDownSuperArcs()) << " down ";
         cout << static_cast<unsigned>(mainNode->getNumberOfUpSuperArcs()) << " up ";
         return;
      }
#endif

      idSuperArc downArc  = mainNode->getDownSuperArcId(0);
      Node *     downNode = getNode((*treeData_.superArcs)[downArc].getDownNodeId());

      downNode->removeUpSuperArc(downArc);
      mainNode->clearDownSuperArcs();

   } else if (mainNode->getNumberOfDownSuperArcs() < 2) {
      // ---------------
      // Have one up arc
      // ---------------

      // We delete the upArc of this node,
      // if there is a down arc, we reattach it to the upNode

      idSuperArc upArc  = mainNode->getUpSuperArcId(0);
      idNode     upId   = (*treeData_.superArcs)[upArc].getUpNodeId();
      Node *     upNode = getNode(upId);

      upNode->removeDownSuperArc(upArc);
      mainNode->clearUpSuperArcs();

      if (mainNode->getNumberOfDownSuperArcs()) {
         // -----------------
         // Have one down arc
         // -----------------

         // Reconnect
         idSuperArc downArc = mainNode->getDownSuperArcId(0);
         (*treeData_.superArcs)[downArc].setUpNodeId(upId);
         upNode->addDownSuperArcId(downArc);
         mainNode->clearDownSuperArcs();

         // Segmentation
         (*treeData_.superArcs)[downArc].concat((*treeData_.superArcs)[upArc]);
      }
   }
#ifndef withKamikaze
   else
      cerr << "delete node with multiple childrens " << endl;
#endif
}

// }
// Segmentation
// ...........................{

void MergeTree::finalizeSegmentation(void)
{
   for (auto &arc : *treeData_.superArcs) {
      arc.createSegmentation(scalars_);
   }
}

//    }
// }
// -------------------------------
// Operators : find, print & clone
// -------------------------------
// {

// Clone
MergeTree *MergeTree::clone() const
{
   MergeTree *newMT = new MergeTree(params_, mesh_, scalars_, treeData_.treeType);

   newMT->treeData_.superArcs = treeData_.superArcs;
   newMT->treeData_.nodes     = treeData_.nodes;
   newMT->treeData_.leaves    = treeData_.leaves;
   newMT->treeData_.roots     = treeData_.roots;
   newMT->treeData_.vert2tree = treeData_.vert2tree;

   return newMT;
}

void MergeTree::clone(const MergeTree *mt)
{
   // we already have common data
   treeData_.superArcs = mt->treeData_.superArcs;
   treeData_.nodes     = mt->treeData_.nodes;
   treeData_.leaves    = mt->treeData_.leaves;
   treeData_.roots     = mt->treeData_.roots;
   treeData_.vert2tree = mt->treeData_.vert2tree;
}

// Print
string MergeTree::printArc(idSuperArc a)
{
   const SuperArc *sa = getSuperArc(a);
   stringstream    res;
   res << a;
   res << " : ";
   res << getNode(sa->getDownNodeId())->getVertexId() << " -- ";
   res << getNode(sa->getUpNodeId())->getVertexId();

   res.seekg(0, ios::end);
   while (res.tellg() < 25) {
      res << " ";
      res.seekg(0, ios::end);
   }
   res.seekg(0, ios::beg);

   res << "segm #" << sa->regionSize() << " / " << scalars_->size;  // << " -> ";

   res.seekg(0, ios::end);

   while (res.tellg() < 45) {
      res << " ";
      res.seekg(0, ios::end);
   }
   res.seekg(0, ios::beg);

   res << sa->printReg();
   return res.str();
}

string MergeTree::printNode(idNode n)
{
   const Node * node = getNode(n);
   stringstream res;
   res << n;
   res << " : (";
   res << node->getVertexId() << ") \\ ";

   for (idSuperArc i = 0; i < node->getNumberOfDownSuperArcs(); ++i) {
      res << "+";
      res << node->getDownSuperArcId(i) << " ";
   }

   res << " / ";

   for (idSuperArc i = 0; i < node->getNumberOfUpSuperArcs(); ++i) {
      res << "+";
      res << node->getUpSuperArcId(i) << " ";
   }

   return res.str();
}

void MergeTree::printTree2()
{
#ifdef withOpenMP
#pragma omp critical
#endif
   {
      cout << "Nodes----------" << endl;
      for (idNode nid = 0; nid < getNumberOfNodes(); nid++) {
         cout << printNode(nid) << endl;
      }

      cout << "Arcs-----------" << endl;
      for (idSuperArc said = 0; said < getNumberOfSuperArcs(); ++said) {
         cout << printArc(said) << endl;
      }

      cout << "Leaves" << endl;
      for (const auto &l : *treeData_.leaves)
         cout << " " << (*treeData_.nodes)[l].getVertexId();
      cout << endl;

      cout << "Roots" << endl;
      for (const auto &r : *treeData_.roots)
         cout << " " << (*treeData_.nodes)[r].getVertexId();
      cout << endl;
   }
}

void MergeTree::printParams(void) const
{
   if (debugLevel_ > 1) {
      cout << "------------" << endl;
      cout << "nb threads : " << threadNumber_ << endl;
      cout << "debug lvl  : " << debugLevel_ << endl;
      cout << "tree type  : ";
      if (params_->treeType == TreeType::Contour) {
         cout << "Contour";
      } else if (params_->treeType == TreeType::Join) {
         cout << "Join";
      } else if (params_->treeType == TreeType::Split) {
         cout << "Split";
      }
      cout << endl;
      cout << "------------" << endl;
   }
}

int MergeTree::printTime(DebugTimer &t, const string &s, idVertex nbScalars, const int debugLevel) const
{
   if (nbScalars == -1) {
      nbScalars = scalars_->size;
   }

   if (debugLevel_ >= debugLevel) {
      stringstream st;
      int          speed = nbScalars / t.getElapsedTime();
      for (int i = 2; i < debugLevel; i++)
         st << "-";
      st << s << " in ";
      st.seekg(0, ios::end);
      while (st.tellg() < 25) {
         st << " ";
         st.seekg(0, ios::end);
      }
      st.seekg(0, ios::beg);
      st << t.getElapsedTime();

      st.seekg(0, ios::end);
      while (st.tellg() < 35) {
         st << " ";
         st.seekg(0, ios::end);
      }
      st.seekg(0, ios::beg);
      st << " at " << speed << " vert/s";
      cout << st.str() << endl;
   }
   return 1;
}

// }

// ##########
// Protected
// ##########

// -----
// Tools
// -----

idNode MergeTree::getVertInRange(const vector<idVertex> &range, const idVertex v,
                                 const idNode last) const
{
    idNode idRes = last;
    const idNode rangeSize = range.size();
    while (idRes+1 < rangeSize && comp_.vertLower(range[idRes + 1], v)) {
       ++idRes;
    }
    return idRes;
}

tuple<idVertex, idVertex> MergeTree::getBoundsFromVerts(const vector<idVertex> &nodes) const
{
    idVertex begin, stop;

    if(isJT()){
       begin = (*scalars_->mirrorVertices)[nodes[0]];
       stop  = scalars_->size;
    } else {
       begin = (*scalars_->mirrorVertices)[nodes[0]];
       stop  = -1;
    }

    return make_tuple(begin, stop);
}

// ---------
// Operators
// ---------

ostream &ttk::operator<<(ostream &o, SuperArc const &a)
{
   o << a.getDownNodeId() << " <>> " << a.getUpNodeId();
   return o;
}

ostream &ttk::operator<<(ostream &o, Node const &n)
{
   o << n.getNumberOfDownSuperArcs() << " .-. " << n.getNumberOfUpSuperArcs();
   return o;
}

