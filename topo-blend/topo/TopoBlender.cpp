#include <set>
#include <QFileSystemModel>

#include "TopoBlender.h"
using namespace Structure;

#include "GraphDistance.h"

#include "ExportDynamicGraph.h"

TopoBlender::TopoBlender(Graph *graph1, Graph *graph2, QObject *parent) : QObject(parent)
{
    g1 = graph1;
    g2 = graph2;

	source = DynamicGraph(g1);
	target = DynamicGraph(g2);
}

void TopoBlender::bestPartialCorrespondence()
{
	QMultiMap<Scalar, QPairNodes> scores;

	/// 1) Compute scores for pairs
	foreach(Node * i, g1->nodes)
	{
		double areaI = i->area();
		Vector3 centerI = i->center();

		foreach(Node * j, g2->nodes)
		{
			// Disabled for now
			if(i->type() != j->type()) continue;

			double score = 0;

			double areaJ = j->area();

			// Score computation
			score += abs( areaI - areaJ );
			score += abs( (centerI - j->center()).norm() );
			score += 1.0 / qMin(areaI, areaJ);
			//score += abs( g1->valence(i) - g2->valence(j) );

			scores.insert(score, qMakePair(i,j));
		}
	}

	/// 2) Find pair with minimal score
	QList<QPairNodes> minPairs;
	double minScore = DBL_MAX;

	foreach(double curScore, scores.keys())
	{
		if(curScore < minScore)
		{
			minScore = curScore;
			minPairs = scores.values(minScore);
		}
	}

	/// 3) Assign "root" node as 'active'
	Node * root = minPairs.first().first;
	Node * targetRoot = minPairs.first().second;

	active = DynamicGraph(g1);

	active.flagNodes("state", SLEEP);
	active.getNode( root->id )->set("correspond", targetRoot->id);
	active.getNode( root->id )->set("state", ACTIVE);
}

QList< ScalarLinksPair > TopoBlender::badCorrespondence(  QString activeNodeID, QString targetNodeID, 
	QMap<Link*, Vec4d> & coord_active, QMap<Link*, Vec4d> & coord_target )
{
	QMap<QPairLink, Scalar> dists;

	coord_active = g1->linksCoords(activeNodeID);
	coord_target = g2->linksCoords(targetNodeID);

	// Compute pair-wise distances [active coords] <-> [target coords]
	foreach(Link * i, coord_active.keys())
	{
		double minDist = DBL_MAX;
		Link * closestLink = NULL;

		foreach(Link * j, coord_target.keys())
		{
			double dist = (coord_active[i] - coord_target[j]).norm();

			if(dist < minDist)
			{
				minDist = dist;
				closestLink = j;
			}
		}

		dists[qMakePair(i, closestLink)] = minDist;
	}

	return sortQMapByValue(dists);
}

Graph TopoBlender::blend(Scalar t)
{
	t = t;

	Graph blendedGraph;

	/// 1) Best partial correspondence
	bestPartialCorrespondence();

	/// 2) BFS solve for Link discrepancy
	Flags flags;

	// Clean up past work
	QDir directory(QDir::currentPath());
	QStringList filesList = directory.entryList(QDir::Files);
	foreach(QString f, filesList) 
	{
		if(f.endsWith(".png") || f.endsWith(".gv") || f.endsWith(".graphml")) 
			directory.remove(f);
	}

	int step = 0;
	QString graphCaption = QString("step%1").arg(step);
	QString graphSubtitle = QString("Initial graph");
	toGraphML(active, graphCaption);
	toGraphviz(active, graphCaption, true, graphCaption, graphSubtitle);

	// Keep track of one-sided links
	QMap< int, int > needLink;
	std::map< int, std::vector<Link> > deadLinks;

	// STAGE 1) Remove extra links + track missing links:

	// while number of 'DONE' nodes is not equal to target
	while((flags = active.flags("state")).count(DONE) != target.nodes.size())
	{
		QString log;

		// Break if we ran out of 'ACTIVE' nodes
		if(flags.indexOf(ACTIVE,0) < 0) 
			break;

		// Find an active node
		int active_idx = active.nodeIndex("state", ACTIVE);
		if(active_idx < 0) break;

		// Get corresponding target node
		SimpleNode & n_active = active.nodes[active_idx];
		QString activeNodeID = n_active.str("original");
		QString targetNodeID = n_active.str("correspond");

		// Get coordinates of all links of both
		QMap<Link*, Vec4d> coord_active, coord_target;
		QList< ScalarLinksPair > corresp = badCorrespondence(activeNodeID, targetNodeID, coord_active, coord_target);

		// Decide either remove links (when target has less links) or add links
		int linkDiff = coord_active.size() - coord_target.size();
		
		// Link abundance:
		if(linkDiff > 0)
		{
			// Find excess links on active node
			for(int i = 0; i < linkDiff; i++)
			{
				// We take away from bottom of sorted list
				Link * link = corresp.takeLast().second.first;
				QString otherNodeID = link->otherNode( activeNodeID )->id;
				int other_idx = active.nodeIndex("original", otherNodeID);

				// Remove the edges
				active.removeEdge(n_active.idx, other_idx);

				// Mark as 'DISCONNECTED' if it is isolated
				if(active.valence(other_idx) == 0)
					active.nodes[other_idx].set("state", DISCONNECTED);

				deadLinks[n_active.idx].push_back(*link);
			}

			// To remaining neighbors, propagate 'ACTIVE' state
			foreach(ScalarLinksPair sp, corresp)
			{
				Link * link = sp.second.first;
				QString otherNodeID = link->otherNode( activeNodeID )->id;
				int other_idx = active.nodeIndex("original", otherNodeID);
				int otherState = active.nodes[other_idx].val("state");

				// Mark as 'ACTIVE'
				if(otherState != DONE && otherState != DISCONNECTED)
					active.nodes[other_idx].set("state", ACTIVE);

				// Set corresponding target node
				Link * linkTarget = sp.second.second;
				QString otherNodeTarget = linkTarget->otherNode(targetNodeID)->id;
				active.nodes[other_idx].set("correspond", otherNodeTarget);
			}

			// Log activity
			log += QString("Removed links from node");
		}

		// link deficiency:
		if(linkDiff < 0)
		{
			// Add special links that need to be filled with [something]
			//	[something] nearby nodes ? 
			needLink[n_active.idx] = abs(linkDiff);

			// propagate 'ACTIVE' state to neighbors
			foreach(ScalarLinksPair sp, corresp)
			{
				Link * link = sp.second.first;
				QString otherNodeID = link->otherNode( activeNodeID )->id;
				int other_idx = active.nodeIndex("original", otherNodeID);
				int otherState = active.nodes[other_idx].val("state");

				// Mark as 'ACTIVE'
				if(otherState != DONE && otherState != DISCONNECTED)
				{
					active.nodes[other_idx].set("state", ACTIVE);

					// Set corresponding target node
					Link * linkTarget = sp.second.second;
					QString otherNodeTarget = linkTarget->otherNode(targetNodeID)->id;
					active.nodes[other_idx].set("correspond", otherNodeTarget);
				}
			}

			log += QString("Need to add links to node");
		}

		// Same links, need to check quality of these links
		if(linkDiff == 0)
		{
			log += QString("Same links count");
		}

		// This node is now 'DONE'
		n_active.set("state", DONE);

		step++;

		QString graphCaption = QString("step%1").arg(step);
		toGraphML(active, graphCaption);
		toGraphviz(active, graphCaption, true, graphCaption, log);
	}

	// STAGE 2) Reconnect for missing links
	foreach(int index, needLink.keys())
	{
		SimpleNode & n_active = active.nodes[index];
		SimpleNode & n_target = *target.getNode(n_active.str("correspond"));

		// get link differences
		QMap<int, SimpleEdge> adjActive = active.getEdges(n_active.idx);
		QMap<int, SimpleEdge> adjTarget = target.getEdges(n_target.idx);

		foreach(int ei, adjActive.keys())
		{
			SimpleNode & ni = active.nodes[adjActive[ei].otherNode(n_active.idx)];

			foreach(int ej, adjTarget.keys())
			{
				SimpleNode & nj = target.nodes[adjTarget[ej].otherNode(n_target.idx)];

				if(ni.property.contains("correspond") && ni.str("correspond") == nj.str("original"))
					adjTarget.remove(ej);
			}
		}

		// Try to connect with dissconnected nodes
		foreach(int edge, adjTarget.keys())
		{
			SimpleNode & n_targetOther = target.nodes[adjTarget[edge].otherNode(n_target.idx)];

			// Get coordinates on the corresponding target node
			Structure::Link * link = target.getOriginalLink(n_target.str("original"), n_targetOther.str("original"));
			Vec4d coordinate = link->getCoord(n_target.str("original"));

			// Get position on active
			Vector3 linkPosition = active.mGraph->getNode(n_active.str("original"))->position(coordinate);

			// Find all distances from link point
			GraphDistance gd(g1);
			gd.computeDistances(linkPosition, 0.25);

			// Store computed distances
			QMap< double, std::pair<int,Vec4d> > dists;

			std::vector<int> dissconnected = active.nodesWith("state", DISCONNECTED);

			// Check : no more dissconnected parts
			if(!dissconnected.size()) break;

			foreach(int d, dissconnected)
			{
				Structure::Node * n = active.mGraph->getNode(active.nodes[d].str("original"));
				
				// Two extremities (based on a coordinate system [0,1])
				double d1 = gd.distanceTo( n->position(n->minCoord()) );
				double d2 = gd.distanceTo( n->position(n->maxCoord()) );

				dists[d1] = std::make_pair(d, n->minCoord());
				dists[d2] = std::make_pair(d, n->maxCoord());
			}

			// Pick closest dissconnected node
			std::pair<int,Vec4d> closestRecord = dists.values().first();
			int closestIdx = closestRecord.first;
			Vec4d closestCoord = closestRecord.second;

			SimpleNode & closest = active.nodes[closestIdx];

			// Set corresponding target node
			closest.set("correspond", n_targetOther.str("original"));
			closest.set("state", ACTIVE);

			// Add a new edge
			active.addEdge(index, closest.idx);

			needLink[index]--;

			// Log event
			QString log = QString("connected node [%1] with [%2]").arg(active.nodes[index].str(
				"original")).arg(active.nodes[closest.idx].str("original"));
			QString graphCaption = QString("step%1").arg(step++);
			toGraphML(active, graphCaption);
			toGraphviz(active, graphCaption, true, graphCaption, log);
		}

	}

	// STAGE 3) Fix missing links for newly connected nodes
	while(active.flags("state").count(ACTIVE))
	{
		// Find an active node
		int active_idx = active.nodeIndex("state", ACTIVE);
		if(active_idx < 0) break;
		SimpleNode & n_active = active.nodes[active_idx];
		QString activeNodeID = n_active.str("original");
		QString targetNodeID = n_active.str("correspond");
		SimpleNode & n_target = target.nodes[target.nodeIndex("original",targetNodeID)];

		// Apply links from corresponding target node
		QMap<int, SimpleEdge> edges = target.getEdges(n_target.idx);
		foreach(int ei, edges.keys())
		{
			QString targetOtherID = target.nodes[edges[ei].otherNode(n_target.idx)].str("original");
			
			int other_idx = active.nodeIndex("correspond",targetOtherID);
			if(other_idx < 0 || active.hasEdge(active_idx, other_idx)) continue;

			// Add a new edge
			active.addEdge(active_idx, other_idx);

			needLink[active_idx]--;

			// Log event
			QString log = QString("connected node [%1] with [%2]").arg(active.nodes[active_idx].str(
				"original")).arg(active.nodes[other_idx].str("original"));
			QString graphCaption = QString("step%1").arg(step++);
			toGraphML(active, graphCaption);
			toGraphviz(active, graphCaption, true, graphCaption, log);
		}

		// Set corresponding target node
		n_active.set("state", DONE);
	}

	// STAGE 4) Grow new nodes if needed


	graphCaption = QString("step%1").arg(++step);
	graphSubtitle = QString("Final graph");
	toGraphML(active, graphCaption);
	toGraphviz(active, graphCaption, true, graphCaption, graphSubtitle);

	// Create animated GIF (assuming ImageMagick installed)
	system(qPrintable( QString("convert -resize 800x800	-delay %1 -loop 0 *.png steps.gif").arg( 200 ) ));

    return blendedGraph;
}
