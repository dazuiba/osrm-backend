#!/bin/bash

# ../scripts/check_performace.sh berlin-latest.osm.pbf 30 >output.csv 2>output.log

OUTPUT=$(mktemp)

function get_times ()
{
    grep -o "[0-9][0-9]*\(\.[0-9][0-9]*\)ms" $OUTPUT | sed s/ms//
}

function wait_for ()
{
    while true ; do
        if grep "$1" $OUTPUT  >& /dev/null ; then
            break
        fi
        sleep 0.5
    done
}

function random()
{
    n=${3:-1}
    awk -v seed=$RANDOM "BEGIN { srand(seed); for(i=0;i<$n;i++) printf(\"%.6f\n\", ($2-$1)*rand() + $1)}"
}

function ch()
{
    local osm_base=${osm_file%%.*}
    local osm=${osm_file/${osm_base}/${osm_base}_ch}
    local osrm=${osm%%.*}.osrm
    if [[ ! -r $osrm ]] ; then
        /bin/cp $osm_file $osm
        >&2 $prefix/osrm-extract -p $profile $osm
        >&2 $prefix/osrm-contract $osrm
    fi
    echo "-a CH $osrm"
}

function corech()
{
    local osm_base=${osm_file%%.*}
    local osm=${osm_file/${osm_base}/${osm_base}_corech}
    local osrm=${osm%%.*}.osrm
    if [[ ! -r $osrm ]] ; then
        /bin/cp $osm_file $osm
        >&2 $prefix/osrm-extract -p $profile $osm
        >&2 $prefix/osrm-contract $osrm --core 0.95
    fi
    echo "-a CoreCH $osrm"
}

function mld()
{
    local osm_base=${osm_file%%.*}
    local osm=${osm_file/${osm_base}/${osm_base}_mld}
    local osrm=${osm%%.*}.osrm
    if [[ ! -r $osrm ]] ; then
        /bin/cp $osm_file $osm
        >&2 $prefix/osrm-extract -p $profile $osm
        >&2 $prefix/osrm-partition $osrm
        >&2 $prefix/osrm-customize $osrm
    fi
    echo "-a MLD $osrm"
}

function run_queries ()
{
    local args=$($1)
    $prefix/osrm-routed $args >& $OUTPUT &
    local pid=$!
    wait_for "running and waiting for requests"

    local result=()
    for ((i=0; i<num_queries; i++)) ; do
        local json=$(curl -s ${queries[$i]})
        result+=( "$(echo $json | jq '.routes[0].weight') $(echo $json | jq '.routes[0].distance')" )
    done
    kill -INT $(pstree $pid -p -a -l | cut -d, -f2 | cut -d' ' -f1)
    wait_for "shutdown completed"

    echo ${result[@]}
}

## variables
prefix='.'
osm_file=${1:-berlin-latest.osm.pbf}
num_queries=${2:-1000}
profile=${3:-../profiles/car.lua}

## get bunding box for queries
>&2 osmium fileinfo $osm_file || exit
bbox=$(osmium fileinfo $osm_file | grep -A 1 "Bounding boxes:" | tail -1 | tr -d ' ()')
IFS=',' read west south east north <<< "$bbox"
>&2 echo -e "Query bounding box ($west,$south)x($east,$north), number of random queries $num_queries"

## construct queries
source_lon=( $(random $west $east $num_queries) )
source_lat=( $(random $south $north $num_queries) )
target_lon=( $(random $west $east $num_queries) )
target_lat=( $(random $south $north $num_queries) )
queries=()
for ((i=0; i<num_queries; i++)) ; do
    queries+=( "http://localhost:5000/route/v1/driving/${source_lon[$i]},${source_lat[$i]};${target_lon[$i]},${target_lat[$i]}" )
done

## run tests
ch_results=( $(run_queries ch) )
ch_times=( $(get_times) )
corech_results=( $(run_queries corech) )
corech_times=( $(get_times) )
mld_results=( $(run_queries mld) )
mld_times=( $(get_times) )

## print results
echo source_lon,source_lat,target_lon,target_lat,\
     ch_weight,ch_distance,ch_query_time,\
     corech_weight,corech_distance,core_ch_query_time,\
     mld_weight,mld_distance,mld_query_time | tr -d ' '

for ((i=0; i<num_queries; i++)) ; do
    echo ${source_lon[i]},${source_lat[i]},${target_lon[i]},${target_lat[i]},\
         ${ch_results[2*i]},${ch_results[2*i+1]},${ch_times[i]},\
         ${corech_results[2*i]},${corech_results[2*i+1]},${corech_times[i]},\
         ${mld_results[2*i]},${mld_results[2*i+1]},${mld_times[i]} | tr -d ' '
done
