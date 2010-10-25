#include "tomahawk/pipeline.h"

#include <QDebug>
#include <QMutexLocker>

#include "tomahawk/functimeout.h"
#include "tomahawk/tomahawkapp.h"
#include "database/database.h"

using namespace Tomahawk;


Pipeline::Pipeline( QObject* parent )
    : QObject( parent )
    , m_index_ready( false )
{

}


void
Pipeline::databaseReady()
{
    connect( APP->database(), SIGNAL(indexReady()), this, SLOT(indexReady()), Qt::QueuedConnection );
    APP->database()->loadIndex();
}


void Pipeline::indexReady()
{
    qDebug() << Q_FUNC_INFO << "shuting this many pending queries:" << m_queries_pending.size();
    m_index_ready = true;
    foreach( const query_ptr& q, m_queries_pending )
    {
        q->setLastPipelineWeight( 101 );
        shunt( q );
    }
    m_queries_pending.clear();
}


void
Pipeline::removeResolver( Resolver* r )
{
    m_resolvers.removeAll( r );
}


void
Pipeline::addResolver( Resolver* r, bool sort )
{
    m_resolvers.append( r );
    if( sort )
    {
        qSort( m_resolvers.begin(),
               m_resolvers.end(),
               Pipeline::resolverSorter );
    }
    qDebug() << "Adding resolver" << r->name();

/*    qDebug() << "Current pipeline:";
    foreach( Resolver * r, m_resolvers )
    {
        qDebug() << "* score:" << r->weight()
                 << "pref:" << r->preference()
                 << "name:" << r->name();
    }*/
}


void
Pipeline::add( const QList<query_ptr>& qlist )
{
    {
        QMutexLocker lock( &m_mut );
        foreach( const query_ptr& q, qlist )
        {
            qDebug() << Q_FUNC_INFO << (qlonglong)q.data() << q->toString();
            if( !m_qids.contains( q->id() ) )
            {
                m_qids.insert( q->id(), q );
            }
        }
    }

    /*
        Since resolvers are async, we now dispatch to the highest weighted ones
        and after timeout, dispatch to next highest etc, aborting when solved

        If index not yet loaded, leave in the pending list instead.
        (they are shunted when index is ready)
     */
    if( m_index_ready )
    {
        foreach( const query_ptr& q, qlist )
        {
            q->setLastPipelineWeight( 101 );
            shunt( q ); // bump into next stage of pipeline (highest weights are 100)
        }
    }
    else
    {
        qDebug() << "Placing query in pending queue - index not ready yet";
        m_queries_pending.append( qlist );
    }
}


void
Pipeline::add( const query_ptr& q )
{
    //qDebug() << Q_FUNC_INFO << (qlonglong)q.data() << q->toString();
    QList< query_ptr > qlist;
    qlist << q;
    add( qlist );
}


void
Pipeline::reportResults( QID qid, const QList< result_ptr >& results )
{
    QMutexLocker lock( &m_mut );

    if( !m_qids.contains( qid ) )
    {
        qDebug() << "reportResults called for unknown QID";
        return;
    }

    const query_ptr& q = m_qids.value( qid );
    //qDebug() << Q_FUNC_INFO << qid;
    //qDebug() << "solved query:" << (qlonglong)q.data() << q->toString();
    q->addResults( results );

    //qDebug() << "Results for " << q->toString() << ", just added" << results.length();
    foreach( const result_ptr& r, q->results() )
    {
        m_rids.insert( r->id(), r );
        //qDebug() << "* " << (results.contains(r) ? "NEW" : "") << r->toString();
    }

}


void
Pipeline::shunt( const query_ptr& q )
{
    if( q->solved() )
    {
        qDebug() << "Query solved, pipeline aborted:" << q->toString()
                << "numresults:" << q->results().length();
        return;
    }
    unsigned int lastweight = 0;
    unsigned int lasttimeout = 0;
    foreach( Resolver* r, m_resolvers )
    {
        if ( r->weight() >= q->lastPipelineWeight() )
            continue;

        if ( lastweight == 0 )
        {
            lastweight = r->weight();
            lasttimeout = r->timeout();
            //qDebug() << "Shunting into weight" << lastweight << "q:" << q->toString();
        }
        if ( lastweight == r->weight() )
        {
            // snag the lowest timeout at this weight
            if ( r->timeout() < lasttimeout )
                lasttimeout = r->timeout();

            // resolvers aren't allowed to block in this call:
            //qDebug() << "Dispaching to resolver" << r->name();
            r->resolve( q->toVariant() );
        }
        else
            break;
    }

    if ( lastweight > 0 )
    {
        q->setLastPipelineWeight( lastweight );
        //qDebug() << "Shunting in" << lasttimeout << "ms, q:" << q->toString();
        new FuncTimeout( lasttimeout, boost::bind( &Pipeline::shunt, this, q ) );
    }
    else
    {
        //qDebug() << "Reached end of pipeline for:" << q->toString();
        // reached end of pipeline
    }
}


bool
Pipeline::resolverSorter( const Resolver* left, const Resolver* right )
{
    if( left->weight() == right->weight() )
        return left->preference() > right->preference();
    else
        return left->weight() > right->weight();
}
