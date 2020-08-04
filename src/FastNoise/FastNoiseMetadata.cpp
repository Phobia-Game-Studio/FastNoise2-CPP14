#include "FastNoise/FastNoiseMetadata.h"
#include "Base64.h"

#include <cassert>
#include <unordered_set>

std::vector<const FastNoise::Metadata*> FastNoise::MetadataManager::sMetadataClasses;


std::string FastNoise::MetadataManager::SerialiseNodeData( NodeData* nodeData, bool fixUp )
{
    std::vector<uint8_t> serialData;
    std::unordered_set<const NodeData*> dependancies;

    if( !SerialiseNodeData( nodeData, serialData, dependancies, fixUp ) )
    {
        return "";
    }
    return Base64::Encode( serialData );
}

template<typename T>
void AddToDataStream( std::vector<uint8_t>& dataStream, T value )
{
    for( size_t i = 0; i < sizeof( T ); i++ )
    {
        dataStream.push_back( (uint8_t)(value >> (i * 8)) );        
    }
}

bool FastNoise::MetadataManager::SerialiseNodeData( NodeData* nodeData, std::vector<uint8_t>& dataStream, std::unordered_set<const NodeData*>& dependancies, bool fixUp )
{
    const Metadata* metadata = nodeData->metadata;

    if( !metadata ||
        nodeData->variables.size() != metadata->memberVariables.size() ||
        nodeData->nodes.size()     != metadata->memberNodes.size()     ||
        nodeData->hybrids.size()   != metadata->memberHybrids.size()   )
    {
        assert( 0 ); // Member size mismatch with metadata
        return false;
    }

    if( fixUp )
    {
        dependancies.insert( nodeData );

        for( auto& node : nodeData->nodes )
        {
            if( dependancies.find( node ) != dependancies.end() )
            {
                node = nullptr;
            }
        }
        for( auto& hybrid : nodeData->hybrids )
        {
            if( dependancies.find( hybrid.first ) != dependancies.end() )
            {
                hybrid.first = nullptr;
            }
        }
    }

    AddToDataStream( dataStream, metadata->id );

    for( size_t i = 0; i < metadata->memberVariables.size(); i++ )
    {
        AddToDataStream( dataStream, nodeData->variables[i].i );
    }

    for( size_t i = 0; i < metadata->memberNodes.size(); i++ )
    {
        if( fixUp && nodeData->nodes[i] )
        {
            std::unique_ptr<Generator> gen( metadata->NodeFactory() );
            SmartNode<> node( nodeData->nodes[i]->metadata->NodeFactory() );

            if( !metadata->memberNodes[i].setFunc( gen.get(), node ) )
            {
                nodeData->nodes[i] = nullptr;
                return false;
            }
        }

        if( !nodeData->nodes[i] || !SerialiseNodeData( nodeData->nodes[i], dataStream, dependancies, fixUp ) )
        {
            return false;
        }
    }

    for( size_t i = 0; i < metadata->memberHybrids.size(); i++ )
    {
        if( !nodeData->hybrids[i].first )
        {
            AddToDataStream( dataStream, (uint8_t)0 );

            Metadata::MemberVariable::ValueUnion v = nodeData->hybrids[i].second;

            AddToDataStream( dataStream, v.i );
        }
        else
        {
            if( fixUp )
            {
                std::unique_ptr<Generator> gen( metadata->NodeFactory() );
                std::shared_ptr<Generator> node( nodeData->hybrids[i].first->metadata->NodeFactory() );

                if( !metadata->memberHybrids[i].setNodeFunc( gen.get(), node ) )
                {
                    nodeData->hybrids[i].first = nullptr;
                    return false;
                }
            }

            AddToDataStream( dataStream, (uint8_t)1 );
            if( !SerialiseNodeData( nodeData->hybrids[i].first, dataStream, dependancies, fixUp ) )
            {
                return false;
            }
        }
    }

    return true; 
}

FastNoise::SmartNode<> FastNoise::MetadataManager::DeserialiseNodeData( const char* serialisedBase64NodeData, FastSIMD::eLevel level )
{
    std::vector<uint8_t> dataStream = Base64::Decode( serialisedBase64NodeData );
    size_t startIdx = 0;

    return DeserialiseNodeData( dataStream, startIdx, level );
}

template<typename T>
bool GetFromDataStream( const std::vector<uint8_t>& dataStream, size_t& idx, T& value )
{
    if( dataStream.size() < idx + sizeof( T ) )
    {
        return false;
    }

    value = *reinterpret_cast<const T*>( dataStream.data() + idx );

    idx += sizeof( T );
    return true;
}

FastNoise::SmartNode<> FastNoise::MetadataManager::DeserialiseNodeData( const std::vector<uint8_t>& serialisedNodeData, size_t& serialIdx, FastSIMD::eLevel level )
{
    uint16_t nodeId;
    if( !GetFromDataStream( serialisedNodeData, serialIdx, nodeId ) )
    {
        return nullptr;
    }

    const Metadata* metadata = GetMetadataClass( nodeId );

    if( !metadata )
    {
        return nullptr;
    }

    SmartNode<> generator( metadata->NodeFactory( level ) );

    for( const auto& var : metadata->memberVariables )
    {
        Metadata::MemberVariable::ValueUnion v;

        if( !GetFromDataStream( serialisedNodeData, serialIdx, v ) )
        {
            return nullptr;
        }

        var.setFunc( generator.get(), v );
    }

    for( const auto& node : metadata->memberNodes )
    {
        SmartNode<> nodeGen = DeserialiseNodeData( serialisedNodeData, serialIdx, level );

        if( !nodeGen || !node.setFunc( generator.get(), nodeGen ) )
        {
            return nullptr;
        }
    }

    for( const auto& hybrid : metadata->memberHybrids )
    {
        uint8_t isGenerator;
        if( !GetFromDataStream( serialisedNodeData, serialIdx, isGenerator ) || isGenerator > 1 )
        {
            return nullptr;
        }

        if( isGenerator )
        {
            SmartNode<> nodeGen = DeserialiseNodeData( serialisedNodeData, serialIdx, level );

            if( !nodeGen || !hybrid.setNodeFunc( generator.get(), nodeGen ) )
            {
                return nullptr;
            }
        }
        else
        {
            float v;

            if( !GetFromDataStream( serialisedNodeData, serialIdx, v ) )
            {
                return nullptr;
            }

            hybrid.setValueFunc( generator.get(), v );
        }
    }

    return generator;
}

#define FASTSIMD_BUILD_CLASS( CLASS ) \
const CLASS::Metadata g ## CLASS ## Metadata( #CLASS );\
const FastNoise::Metadata* CLASS::GetMetadata()\
{\
    return &g ## CLASS ## Metadata;\
}\
Generator* CLASS::Metadata::NodeFactory( FastSIMD::eLevel l ) const\
{\
    return FastSIMD::New<CLASS>( l );\
}

#define FASTSIMD_INCLUDE_HEADER_ONLY
#include "FastNoise/FastNoise_BuildList.inl"