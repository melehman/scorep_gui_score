/*
 * This file is part of the Score-P software (http://www.score-p.org)
 *
 * Copyright (c) 2009-2013,
 * RWTH Aachen University, Germany
 *
 * Copyright (c) 2009-2013,
 * Gesellschaft fuer numerische Simulation mbH Braunschweig, Germany
 *
 * Copyright (c) 2009-2014,
 * Technische Universitaet Dresden, Germany
 *
 * Copyright (c) 2009-2013,
 * University of Oregon, Eugene, USA
 *
 * Copyright (c) 2009-2015,
 * Forschungszentrum Juelich GmbH, Germany
 *
 * Copyright (c) 2009-2013, 2015,
 * German Research School for Simulation Sciences GmbH, Juelich/Aachen, Germany
 *
 * Copyright (c) 2009-2013,
 * Technische Universitaet Muenchen, Germany
 *
 * This software may be modified and distributed under the terms of
 * a BSD-style license.  See the COPYING file in the package base
 * directory for details.
 *
 */

/**
 * @file
 *
 * @brief      Implements a class which performs calculations for trace
 *             size estimation.
 */

#include "SCOREP_Score_Estimator.hpp"
#include "SCOREP_Score_EventList.hpp"
#include "SCOREP_Score_Types.hpp"
#include <math.h>
#include <fstream>
#include <iomanip>
#include <QString>
#include <QDebug>

using namespace std;

#define SCOREP_SCORE_BUFFER_SIZE 128

/* **************************************************************************************
                                                                       internal functions
****************************************************************************************/

/**
 * Swaps the items on @a pos1 and @a pos2 from the group list.
 * Needed for the quicksort.
 * @param item  List of groups that is sorted.
 * @param pos1  Position of an elemant that is swapped with the element at @a pos2.
 * @param pos2  Position of an elemant that is swapped with the element at @a pos1.
 */
static void
swap( SCOREP_Score_Group** items, uint64_t pos1, uint64_t pos2 )
{
    SCOREP_Score_Group* helper = items[ pos1 ];
    items[ pos1 ] = items[ pos2 ];
    items[ pos2 ] = helper;
}

/**
 * Sorts an array of pointers to groups using the quicksort algorithm.
 * @param items  Array of groups that are sorted.
 * @param size   Number of groups in @a items.
 */
static void
quicksort( SCOREP_Score_Group** items, uint64_t size )
{
    if ( size < 2 )
    {
        return;
    }
    if ( size == 2 )
    {
        if ( items[ 0 ]->getMaxTraceBufferSize() <
             items[ 1 ]->getMaxTraceBufferSize() )
        {
            swap( items, 0, 1 );
        }
        return;
    }

    uint64_t beg       = 0;
    uint64_t end       = size - 1;
    uint64_t pos       = size / 2;
    uint64_t threshold = items[ pos ]->getMaxTraceBufferSize();

    while ( beg < end )
    {
        while ( ( beg < end ) &&
                ( items[ beg ]->getMaxTraceBufferSize() > threshold ) )
        {
            beg++;
        }

        while ( ( beg < end ) &&
                ( items[ end ]->getMaxTraceBufferSize() <= threshold ) )
        {
            end--;
        }
        if ( beg < end )
        {
            swap( items, beg, end );

            // Maintain position of our threshold item
            // Needed for special handling of equal values
            if ( beg == pos )
            {
                pos = end;
            }
            else if ( end == pos )
            {
                pos = beg;
            }
        }
    }

    // Special handling if we have many entries with the same value
    // Otherwise,lots of equal values might lead to infinite recursion.
    if ( items[ end ]->getMaxTraceBufferSize() < threshold )
    {
        swap( items, end, pos );
    }
    while ( ( end < size ) &&
            ( items[ end ]->getMaxTraceBufferSize() == threshold ) )
    {
        end++;
    }

    quicksort( items, beg );
    quicksort( &items[ end ], size - end );
}

static bool
is_writable_file_creatable( string filename )
{
    fstream temp_file( filename.c_str(), ios_base::out );
    if ( temp_file.good() )
    {
        temp_file.close();
        remove( filename.c_str() );
        return true;
    }
    return false;
}

static string
get_temp_filename( void )
{
    static string      temp_dir;
    static bool        is_initialized = false;
    static const char* username       = getenv( "USER" );
    static uint32_t    sequence_no    = 0;

    stringstream filename;
    filename << "scorep-score_"; // Avoid clashes with other applications
    if ( username != NULL )
    {
        filename << username << "_"; // Avoid interference with other users
    }
    filename << getpid() << "_"      // Avoid interference with other process of the same user
             << sequence_no;         // Avoid interference of multiple calls in the one tool

    sequence_no++;

    if ( !is_initialized )
    {
        static string slash = "/";
        const char*   temp;
        is_initialized = true;

        temp = getenv( "TMPDIR" );
        if ( ( temp != NULL ) &&
             is_writable_file_creatable( temp + slash + filename.str() ) )
        {
            temp_dir = temp + slash;
            return temp_dir + filename.str();
        }

        temp = getenv( "TMP" );
        if ( ( temp != NULL ) &&
             is_writable_file_creatable( temp + slash + filename.str() ) )
        {
            temp_dir = temp + slash;
            return temp_dir + filename.str();
        }

        temp = getenv( "TEMP" );
        if ( ( temp != NULL ) &&
             is_writable_file_creatable( temp + slash + filename.str() ) )
        {
            temp_dir = temp + slash;
            return temp_dir + filename.str();
        }

        temp_dir = "/tmp/";
        if ( is_writable_file_creatable( temp_dir + filename.str() ) )
        {
            return temp_dir + filename.str();
        }

        temp_dir = "";
        if ( is_writable_file_creatable( filename.str() ) )
        {
            return filename.str();
        }

        cerr << "ERROR: Failed to find writable directory for temporary files." << endl;
        exit( EXIT_FAILURE );
    }

    return temp_dir + filename.str();
}

static string
get_user_readable_byte_no( uint64_t bytes )
{
    const uint32_t base = 1024;
    uint32_t       exp;
    for ( exp = 0; bytes > 4 * base; exp++ )
    {
        bytes /= base;
    }
    // round up;
    bytes++;
    stringstream result;
    result << bytes;
    switch ( exp )
    {
        case 0:
            result << " bytes";
            break;
        case 1:
            result << "kB";
            break;
        case 2:
            result << "MB";
            break;
        case 3:
            result << "GB";
            break;
        case 4:
            result << "TB";
            break;
        case 5:
            result << "PB";
            break;
        case 6:
            result << "EB";
            break;
        case 7:
            result << "ZB";
            break;
        case 8:
            result << "YB";
            break;
    }
    return result.str();
}


/* **************************************************************************************
                                                             class SCOREP_Score_Estimator
****************************************************************************************/

SCOREP_Score_Estimator::SCOREP_Score_Estimator( std::string fileName,
                                                uint64_t    denseNum )
{
    m_dense_num = denseNum;
    SCOREP_Score_Profile* profile;
    try
    {
        profile = new SCOREP_Score_Profile( fileName );
    }
    catch ( ... )
    {
        /*ERROR*/
        return;
    }

    m_profile     = profile;
    m_region_num  = profile->getNumberOfRegions();
    m_process_num = profile->getNumberOfProcesses();

    m_has_filter = false;
    SCOREP_Score_Event::RegisterEvent( new SCOREP_Score_TimestampEvent() );
    SCOREP_Score_Event::RegisterEvent( new SCOREP_Score_EnterEvent() );
    SCOREP_Score_Event::RegisterEvent( new SCOREP_Score_LeaveEvent() );
    if ( denseNum > 0 )
    {
        SCOREP_Score_Event::RegisterEvent( new SCOREP_Score_MetricEvent( denseNum ) );
    }
    SCOREP_Score_Event::RegisterEvent( new SCOREP_Score_ParameterEvent() );

#define SCOREP_SCORE_EVENT( name ) region_set.insert( name );
    set<string> region_set;
    SCOREP_SCORE_EVENT_MPI_SEND;
    SCOREP_Score_Event::RegisterEvent( new SCOREP_Score_NameMatchEvent( "MpiSend", region_set ) );

    region_set.clear();
    SCOREP_SCORE_EVENT_MPI_ISEND;
    SCOREP_Score_Event::RegisterEvent( new SCOREP_Score_NameMatchEvent( "MpiIsend", region_set ) );

    region_set.clear();
    SCOREP_SCORE_EVENT_MPI_ISENDCOMPLETE;
    SCOREP_Score_Event::RegisterEvent( new SCOREP_Score_NameMatchEvent( "MpiIsendComplete",
                                                                        region_set ) );

    region_set.clear();
    SCOREP_SCORE_EVENT_MPI_IRECVREQUEST;
    SCOREP_Score_Event::RegisterEvent( new SCOREP_Score_NameMatchEvent( "MpiIrecvRequest",
                                                                        region_set ) );

    region_set.clear();
    SCOREP_SCORE_EVENT_MPI_RECV;
    SCOREP_Score_Event::RegisterEvent( new SCOREP_Score_NameMatchEvent( "MpiRecv", region_set ) );

    region_set.clear();
    SCOREP_SCORE_EVENT_MPI_IRECV;
    SCOREP_Score_Event::RegisterEvent( new SCOREP_Score_NameMatchEvent( "MpiIrecv", region_set ) );

    region_set.clear();
    SCOREP_SCORE_EVENT_MPI_COLLECTIVE;
    SCOREP_Score_Event::RegisterEvent( new SCOREP_Score_NameMatchEvent( "MpiCollectiveBegin",
                                                                        region_set ) );
    SCOREP_Score_Event::RegisterEvent( new SCOREP_Score_NameMatchEvent( "MpiCollectiveEnd",
                                                                        region_set ) );

    region_set.clear();
    SCOREP_SCORE_EVENT_THREAD_ACQUIRELOCK;
    SCOREP_Score_Event::RegisterEvent( new SCOREP_Score_NameMatchEvent( "ThreadAcquireLock",
                                                                        region_set ) );

    region_set.clear();
    SCOREP_SCORE_EVENT_THREAD_RELEASELOCK;
    SCOREP_Score_Event::RegisterEvent( new SCOREP_Score_NameMatchEvent( "ThreadReleaseLock",
                                                                        region_set ) );

    region_set.clear();
    SCOREP_SCORE_EVENT_RMA_OP;
    SCOREP_Score_Event::RegisterEvent( new SCOREP_Score_NameMatchEvent( "RmaPut",
                                                                        region_set ) );
    SCOREP_Score_Event::RegisterEvent( new SCOREP_Score_NameMatchEvent( "RmaOpCompleteBlocking",
                                                                        region_set ) );

    region_set.clear();
    SCOREP_SCORE_EVENT_RMA_OP_COMPLETE_REMOTE;
    SCOREP_Score_Event::RegisterEvent( new SCOREP_Score_NameMatchEvent( "RmaOpCompleteRemote",
                                                                        region_set ) );

    region_set.clear();
    SCOREP_SCORE_EVENT_RMA_ATOMIC;
    SCOREP_Score_Event::RegisterEvent( new SCOREP_Score_NameMatchEvent( "RmaAtomic",
                                                                        region_set ) );
    SCOREP_Score_Event::RegisterEvent( new SCOREP_Score_NameMatchEvent( "RmaOpCompleteBlocking",
                                                                        region_set ) );

    region_set.clear();
    SCOREP_SCORE_EVENT_RMA_COLLECTIVE;
    SCOREP_Score_Event::RegisterEvent( new SCOREP_Score_NameMatchEvent( "RmaCollectiveBegin",
                                                                        region_set ) );
    SCOREP_Score_Event::RegisterEvent( new SCOREP_Score_NameMatchEvent( "RmaCollectiveEnd",
                                                                        region_set ) );

    region_set.clear();
    SCOREP_SCORE_EVENT_RMA_WAIT_CHANGE;
    SCOREP_Score_Event::RegisterEvent( new SCOREP_Score_NameMatchEvent( "RmaWaitChange",
                                                                        region_set ) );

    region_set.clear();
    SCOREP_SCORE_EVENT_RMA_LOCK;
    SCOREP_Score_Event::RegisterEvent( new SCOREP_Score_NameMatchEvent( "RmaRequestLock",
                                                                        region_set ) );

    region_set.clear();
    SCOREP_SCORE_EVENT_RMA_RELEASE_LOCK;
    SCOREP_Score_Event::RegisterEvent( new SCOREP_Score_NameMatchEvent( "RmaReleaseLock",
                                                                        region_set ) );

#undef SCOREP_SCORE_EVENT
#define SCOREP_SCORE_EVENT( name ) region_list.push_back( name );
    deque<string> region_list;
    region_list.clear();
    SCOREP_SCORE_EVENT_THREAD_FORK;
    SCOREP_Score_Event::RegisterEvent( new SCOREP_Score_PrefixMatchEvent( "ThreadFork",
                                                                          region_list ) );

    region_list.clear();
    SCOREP_SCORE_EVENT_THREAD_JOIN;
    SCOREP_Score_Event::RegisterEvent( new SCOREP_Score_PrefixMatchEvent( "ThreadJoin",
                                                                          region_list ) );

    region_list.clear();
    SCOREP_SCORE_EVENT_THREAD_TEAM;
    SCOREP_Score_Event::RegisterEvent( new SCOREP_Score_PrefixMatchEvent( "ThreadTeamBegin",
                                                                          region_list ) );
    SCOREP_Score_Event::RegisterEvent( new SCOREP_Score_PrefixMatchEvent( "ThreadTeamEnd",
                                                                          region_list ) );

    region_list.clear();
    SCOREP_SCORE_EVENT_THREAD_TASK_CREATE;
    SCOREP_Score_Event::RegisterEvent( new SCOREP_Score_PrefixMatchEvent( "ThreadTaskCreate",
                                                                          region_list ) );
    SCOREP_Score_Event::RegisterEvent( new SCOREP_Score_PrefixMatchEvent( "ThreadTaskComplete",
                                                                          region_list ) );

    region_list.clear();
    SCOREP_SCORE_EVENT_THREAD_TASK_SWITCH;
    SCOREP_Score_Event::RegisterEvent( new SCOREP_Score_PrefixMatchEvent( "ThreadTaskSwitch",
                                                                          region_list ) );
    region_list.clear();
    SCOREP_SCORE_EVENT_THREAD_CREATE_WAIT_CREATE;
    SCOREP_Score_Event::RegisterEvent( new SCOREP_Score_PrefixMatchEvent( "ThreadCreate",
                                                                          region_list ) );

    region_list.clear();
    SCOREP_SCORE_EVENT_THREAD_CREATE_WAIT_BEGIN;
    SCOREP_Score_Event::RegisterEvent( new SCOREP_Score_PrefixMatchEvent( "ThreadBegin",
                                                                          region_list ) );

    region_list.clear();
    SCOREP_SCORE_EVENT_THREAD_CREATE_WAIT_WAIT;
    SCOREP_Score_Event::RegisterEvent( new SCOREP_Score_PrefixMatchEvent( "ThreadWait",
                                                                          region_list ) );

    region_list.clear();
    SCOREP_SCORE_EVENT_THREAD_CREATE_WAIT_END;
    SCOREP_Score_Event::RegisterEvent( new SCOREP_Score_PrefixMatchEvent( "ThreadEnd",
                                                                          region_list ) );
#undef SCOREP_SCORE_EVENT

    calculate_event_sizes();

    m_filtered = NULL;
    m_regions  = NULL;
    m_groups   = ( SCOREP_Score_Group** )malloc( SCOREP_SCORE_TYPE_NUM * sizeof( SCOREP_Score_Group* ) );
    for ( uint64_t i = 0; i < SCOREP_SCORE_TYPE_NUM; i++ )
    {
        m_groups[ i ] = new SCOREP_Score_Group( i, m_process_num,
                                                SCOREP_Score_getTypeName( i ) );
    }
}

SCOREP_Score_Estimator::~SCOREP_Score_Estimator()
{
    delete_groups( m_groups, SCOREP_SCORE_TYPE_NUM );
    delete_groups( m_regions, m_region_num );
    delete_groups( m_filtered, SCOREP_SCORE_TYPE_NUM );
}

void
SCOREP_Score_Estimator::initializeFilter( string /*filterFile*/ )
{
    /* Initialize filter component */

    /* Initialize filter groups */
    m_filtered = ( SCOREP_Score_Group** )
                 malloc( SCOREP_SCORE_TYPE_NUM * sizeof( SCOREP_Score_Group* ) );

    for ( uint64_t i = 0; i < SCOREP_SCORE_TYPE_NUM; i++ )
    {
        string name = SCOREP_Score_getTypeName( i );
        if ( i != SCOREP_SCORE_TYPE_FLT )
        {
            name += "-FLT";
        }

        m_filtered[ i ] = new SCOREP_Score_Group( i, m_process_num, name );
        m_filtered[ i ]->doFilter( SCOREP_Score_getFilterState( i ) );
    }

    /* Update regular groups */
    for ( uint64_t i = 0; i < SCOREP_SCORE_TYPE_NUM; i++ )
    {
        m_groups[ i ]->doFilter( SCOREP_SCORE_FILTER_NO );
    }

    m_has_filter = true;
}

void
SCOREP_Score_Estimator::calculate( bool showRegions, bool useMangled, QHash<QString, QHash<int, dataCenter::buffer> >* buffer )
{
    if ( showRegions )
    {
        initialize_regions( useMangled );
    }

    uint64_t temp1 = 0;
    uint64_t temp0 = 0;
    for ( uint64_t region = 0; region < m_region_num; region++ )
    {
        const string& region_name     = m_profile->getRegionName( region );
        uint64_t      group           = m_profile->getGroup( region );
        uint64_t      bytes_per_visit = 0;

        /* Calculate bytes per visit */
        for ( map<string, SCOREP_Score_Event*>::iterator i = SCOREP_Score_Event::m_all_events.begin();
              i != SCOREP_Score_Event::m_all_events.end(); i++ )
        {
            if ( i->second->occursInRegion( region_name ) )
            {
                bytes_per_visit += i->second->getEventSize();
            }
        }
        QHash<int, dataCenter::buffer> tempHash;
        /* Apply region data for each process */
        for ( uint64_t process = 0; process < m_process_num; process++ )
        {
            dataCenter::buffer tempStruct;
            uint64_t           visits = m_profile->getVisits( region, process );
            double             time   = m_profile->getTime( region, process );

            if ( visits == 0 )
            {
                continue;
            }
            tempStruct.bytesPerVisit  = bytes_per_visit;
            tempStruct.numberOfVisits = visits;
            tempHash.insert( process, tempStruct );
            if ( process == 0 )
            {
                temp0 += visits * bytes_per_visit;
            }
            else if ( process == 1 )
            {
                temp1 += visits * bytes_per_visit;
            }
            m_groups[ group ]->addRegion( visits, bytes_per_visit, time, process );
            m_groups[ SCOREP_SCORE_TYPE_ALL ]->addRegion( visits, bytes_per_visit,
                                                          time, process );

            if ( showRegions )
            {
                m_regions[ region ]->addRegion( visits, bytes_per_visit, time, process );
            }

            if ( m_has_filter )
            {
                bool do_filter = match_filter( region );
                if ( showRegions )
                {
                    m_regions[ region ]->doFilter( do_filter ?
                                                   SCOREP_SCORE_FILTER_YES :
                                                   SCOREP_SCORE_FILTER_NO );
                }
                if ( !do_filter )
                {
                    m_filtered[ group ]->addRegion( visits, bytes_per_visit,
                                                    time, process );
                    m_filtered[ SCOREP_SCORE_TYPE_ALL ]->addRegion( visits,
                                                                    bytes_per_visit,
                                                                    time, process );
                }
                else
                {
                    m_filtered[ SCOREP_SCORE_TYPE_FLT ]->addRegion( visits,
                                                                    bytes_per_visit,
                                                                    time, process );
                }
            }
        }
        if ( tempHash.size() != 0 )
        {
            buffer->insert( QString::fromStdString( region_name ), tempHash );
        }
        tempHash.clear();
    }
}

void
SCOREP_Score_Estimator::printGroups( void )
{
    double   total_time = m_groups[ SCOREP_SCORE_TYPE_ALL ]->getTotalTime();
    uint64_t max_buf;
    uint64_t total_buf;
    uint64_t memory_req;
    uint64_t value = 2 * 1024 * 1024;
    if ( m_has_filter )
    {
        max_buf   = m_filtered[ SCOREP_SCORE_TYPE_ALL ]->getMaxTraceBufferSize();
        total_buf = m_filtered[ SCOREP_SCORE_TYPE_ALL ]->getTotalTraceBufferSize();
    }
    else
    {
        max_buf   = m_groups[ SCOREP_SCORE_TYPE_ALL ]->getMaxTraceBufferSize();
        total_buf = m_groups[ SCOREP_SCORE_TYPE_ALL ]->getTotalTraceBufferSize();
    }

    /* Estimate definition size by profile size and add a minimum of 4 MB. */
    memory_req = m_profile->getFileSize() /
                 ( m_profile->getNumberOfProcesses() * m_profile->getNumberOfMetrics() );
    memory_req = max_buf + memory_req;
    memory_req = value > memory_req ? value : memory_req;
    memory_req = memory_req + value *  m_profile->getMaxNumberOfLocationsPerProcess();

    cout << endl;
    cout << "Estimated aggregate size of event trace:                   "
         << get_user_readable_byte_no( total_buf ) << endl;
    cout << "Estimated requirements for largest trace buffer (max_buf): "
         << get_user_readable_byte_no( max_buf ) << endl;
    cout << "Estimated memory requirements (SCOREP_TOTAL_MEMORY):       "
         << get_user_readable_byte_no( memory_req ) << endl;
    cout << "(hint: When tracing set SCOREP_TOTAL_MEMORY="
         << get_user_readable_byte_no( memory_req ) << " to avoid intermediate flushes\n"
         << " or reduce requirements using USR regions filters.)"
         << endl << endl;

    quicksort( m_groups, SCOREP_SCORE_TYPE_NUM );

    // The first group is "ALL"
    m_groups[ 0 ]->updateWidths( m_widths );
    cout << "flt"
         << " " << setw( m_widths.m_type ) << "type"
         << " " << setw( m_widths.m_bytes ) << "max_buf[B]"
         << " " << setw( m_widths.m_visits ) << "visits"
         << " " << setw( m_widths.m_time ) << "time[s]"
         << " time[%]"
         << " " << setw( m_widths.m_time_per_visit ) << "time/visit[us]"
         << "  region"
         << endl;
    for ( uint64_t i = 0; i < SCOREP_SCORE_TYPE_NUM; i++ )
    {
        m_groups[ i ]->print( total_time, m_widths );
    }

    if ( m_has_filter )
    {
        quicksort( &m_filtered[ 1 ], SCOREP_SCORE_TYPE_NUM - 1 );

        cout << endl;
        for ( uint64_t i = 0; i < SCOREP_SCORE_TYPE_NUM; i++ )
        {
            m_filtered[ i ]->print( total_time, m_widths );
        }
    }
}

void
SCOREP_Score_Estimator::printRegions( void )
{
    quicksort( m_regions, m_region_num );

    double total_time = m_groups[ SCOREP_SCORE_TYPE_ALL ]->getTotalTime();
    cout << endl;
    for ( uint64_t i = 0; i < m_region_num; i++ )
    {
        m_regions[ i ]->print( total_time, m_widths );
    }
}

void
SCOREP_Score_Estimator::dumpEventSizes( void )
{
    for ( map<string, SCOREP_Score_Event*>::iterator i = SCOREP_Score_Event::m_all_events.begin(); i != SCOREP_Score_Event::m_all_events.end(); i++ )
    {
        const string& name   = i->second->getName();
        string        blanks = "                         ";
        cerr << name << ":" << blanks.substr( 0, 20 - name.length() )
             << i->second->getEventSize() << endl;
    }
}

void
SCOREP_Score_Estimator::calculate_event_sizes( void )
{
    /* Write otf2-estimator input */
    string in_filename  = get_temp_filename();
    string out_filename = get_temp_filename();

    fstream estimator_in( in_filename.c_str(), ios_base::out );
    if ( !estimator_in )
    {
        cerr << "ERROR: Failed to open temorary file for otf2-estimator input."
             << endl;
        exit( EXIT_FAILURE );
    }

    estimator_in << "set Region " << m_region_num << "\n";
    estimator_in << "set Metric " << m_profile->getNumberOfMetrics() << "\n";

    for ( map<string, SCOREP_Score_Event*>::iterator i = SCOREP_Score_Event::m_all_events.begin(); i != SCOREP_Score_Event::m_all_events.end(); i++ )
    {
        estimator_in << "get " << i->second->getName() << "\n";
    }
    estimator_in << "exit" << endl;
    estimator_in.close();

    string command = "otf2-estimator" " > \"" +
                     out_filename + "\" < \"" + in_filename + "\"";
    if ( system( command.c_str() ) != EXIT_SUCCESS )
    {
        cerr << "ERROR: Failed to call otf2-estimator." << endl;
        return;
    }

    /* Read output of otf2-estimator */
    fstream estimator_out( out_filename.c_str(), ios_base::in );
    if ( !estimator_out )
    {
        cerr << "ERROR: Failed to open temorary file for otf2-estimator input."
             << endl;
        exit( EXIT_FAILURE );
    }

    while ( estimator_out )
    {
        /* Decode next line. Has format <name><space><number of bytes>
         * where <name> can have spaces too. */
        string line;
        getline( estimator_out, line );
        size_t size_pos = line.find_last_of( " " );
        if ( size_pos == string::npos )
        {
            continue;
        }
        string   event = line.substr( 0, size_pos );
        string   number( line.substr( size_pos + 1 ) );
        char*    end_pos;
        uint64_t value = strtoul( number.c_str(), &end_pos, 0 );
        if ( *end_pos )
        {
            continue;
        }

        /* Apply to event sizes */
        SCOREP_Score_Event::SetEventSize( event, value );
    }

    /* Clean up */
    estimator_out.close();
    remove( in_filename.c_str() );
    remove( out_filename.c_str() );

    //dumpEventSizes();
}

/* ****************************************************** private methods */

void
SCOREP_Score_Estimator::delete_groups( SCOREP_Score_Group** groups, uint64_t num )
{
    if ( groups != NULL )
    {
        for ( uint64_t i = 0; i < num; i++ )
        {
            delete ( groups[ i ] );
        }
        free( groups );
    }
}

void
SCOREP_Score_Estimator::initialize_regions( bool useMangled )
{
    int size = sizeof( SCOREP_Score_Group* );
    m_regions = ( SCOREP_Score_Group** )malloc( m_region_num * size );
    for ( uint64_t region = 0; region < m_region_num; region++ )
    {
        string name = ( useMangled ?
                        m_profile->getMangledName( region ) :
                        m_profile->getRegionName( region ) );

        m_regions[ region ] = new SCOREP_Score_Group( m_profile->getGroup( region ),
                                                      m_process_num,
                                                      m_profile->getRegionName( region ),
                                                      m_profile->getMangledName( region ) );
    }
}

bool
SCOREP_Score_Estimator::match_filter( uint64_t /*region*/ )
{
    /* bool do_filter = SCOREP_Filter_Match( m_profile->getFileName( region ).c_str(),
                                           m_profile->getRegionName( region ).c_str(),
                                           m_profile->getMangledName( region ).c_str() );
       return do_filter &&
            SCOREP_Score_getFilterState( m_profile->getGroup( region ) ) != SCOREP_SCORE_FILTER_NO;*/
    return true;
}

void
SCOREP_Score_Estimator::getSizes( uint64_t* traceSize, uint64_t* maxBuf, uint64_t* totalMemory )
{
    uint64_t memory_req;
    uint64_t value = 2 * 1024 * 1024;
    *maxBuf    = m_groups[ SCOREP_SCORE_TYPE_ALL ]->getMaxTraceBufferSize();
    *traceSize = m_groups[ SCOREP_SCORE_TYPE_ALL ]->getTotalTraceBufferSize();

    memory_req = m_profile->getFileSize() /
                 ( m_profile->getNumberOfProcesses() * m_profile->getNumberOfMetrics() );
    memory_req   = *maxBuf + memory_req;
    memory_req   = value > memory_req ? value : memory_req;
    memory_req   = memory_req + value *  m_profile->getMaxNumberOfLocationsPerProcess();
    *totalMemory = memory_req;
}

dataCenter::groupData
SCOREP_Score_Estimator::getGroupInformation( int number )
{
    quicksort( m_groups, SCOREP_SCORE_TYPE_NUM );
    dataCenter::groupData d;
    double                total_time = m_groups[ SCOREP_SCORE_TYPE_ALL ]->getTotalTime();
    m_groups[ number ]->getGroupData( &d.type, &d.maxBuf, &d.visits,
                                      &d.timeS, &d.timeP, &d.timePerVisit,
                                      &d.region, total_time );
    d.key   = number;
    d.state = dataCenter::INCLUDED;
    return d;
}

dataCenter::data
SCOREP_Score_Estimator::getRegionInformation( int number )
{
    quicksort( m_regions, m_region_num );
    dataCenter::data d;
    double           total_time = m_groups[ SCOREP_SCORE_TYPE_ALL ]->getTotalTime();
    m_regions[ number ]->getRegionData( &d.type, &d.maxBuf, &d.visits,
                                        &d.timeS, &d.timeP, &d.timePerVisit,
                                        &d.region, total_time, &d.mangledName );
    d.key      = number;
    d.included = true;
    return d;
}

uint64_t
SCOREP_Score_Estimator::getTypeNum()
{
    return SCOREP_SCORE_TYPE_NUM;
}

uint64_t
SCOREP_Score_Estimator::getRegionNum()
{
    return m_region_num;
}

uint64_t
SCOREP_Score_Estimator::updateMemory( uint64_t maxBuf )
{
    uint64_t memory_req;
    uint64_t value = 2 * 1024 * 1024;
    /* Estimate definition size by profile size and add a minimum of 4 MB. */
    memory_req = m_profile->getFileSize() /
                 ( m_profile->getNumberOfProcesses() * m_profile->getNumberOfMetrics() );
    memory_req = maxBuf + memory_req;
    memory_req = value > memory_req ? value : memory_req;
    memory_req = memory_req + value *  m_profile->getMaxNumberOfLocationsPerProcess();
    return memory_req;
}
