#pragma once

#include <DB/Dictionaries/IDictionary.h>
#include <DB/Dictionaries/IDictionarySource.h>
#include <DB/Dictionaries/DictionaryStructure.h>
#include <DB/Columns/ColumnString.h>
#include <statdaemons/ext/range.hpp>
#include <vector>
#include <tuple>

namespace DB
{

const auto initial_array_size = 1024;
const auto max_array_size = 500000;

class FlatDictionary final : public IDictionary
{
public:
    FlatDictionary(const std::string & name, const DictionaryStructure & dict_struct,
		DictionarySourcePtr source_ptr, const DictionaryLifetime dict_lifetime)
		: name{name}, dict_struct(dict_struct),
		  source_ptr{std::move(source_ptr)}, dict_lifetime(dict_lifetime)
	{
		createAttributes();
		loadData();
	}

	FlatDictionary(const FlatDictionary & other)
		: FlatDictionary{other.name, other.dict_struct, other.source_ptr->clone(), other.dict_lifetime}
	{}

	std::string getName() const override { return name; }

	std::string getTypeName() const override { return "FlatDictionary"; }

	bool isCached() const override { return false; }

	DictionaryPtr clone() const override { return std::make_unique<FlatDictionary>(*this); }

	const IDictionarySource * getSource() const override { return source_ptr.get(); }

	const DictionaryLifetime & getLifetime() const override { return dict_lifetime; }

	bool hasHierarchy() const override { return hierarchical_attribute; }

	id_t toParent(const id_t id) const override
	{
		const auto attr = hierarchical_attribute;
		const auto & array = *std::get<std::unique_ptr<PODArray<UInt64>>>(attr->arrays);

		return id < array.size() ? array[id] : std::get<UInt64>(attr->null_values);
	}

	void toParent(const PODArray<id_t> & ids, PODArray<id_t> & out) const override
	{
		getItems<UInt64>(*hierarchical_attribute, ids, out);
	}

#define DECLARE_INDIVIDUAL_GETTER(TYPE, LC_TYPE) \
	TYPE get##TYPE(const std::string & attribute_name, const id_t id) const override\
	{\
		const auto & attribute = getAttribute(attribute_name);\
		if (attribute.type != AttributeType::LC_TYPE)\
			throw Exception{\
				"Type mismatch: attribute " + attribute_name + " has type " + toString(attribute.type),\
				ErrorCodes::TYPE_MISMATCH\
			};\
		\
		const auto & array = *std::get<std::unique_ptr<PODArray<TYPE>>>(attribute.arrays);\
		\
		return id < array.size() ? array[id] : std::get<TYPE>(attribute.null_values);\
	}
	DECLARE_INDIVIDUAL_GETTER(UInt8, uint8)
	DECLARE_INDIVIDUAL_GETTER(UInt16, uint16)
	DECLARE_INDIVIDUAL_GETTER(UInt32, uint32)
	DECLARE_INDIVIDUAL_GETTER(UInt64, uint64)
	DECLARE_INDIVIDUAL_GETTER(Int8, int8)
	DECLARE_INDIVIDUAL_GETTER(Int16, int16)
	DECLARE_INDIVIDUAL_GETTER(Int32, int32)
	DECLARE_INDIVIDUAL_GETTER(Int64, int64)
	DECLARE_INDIVIDUAL_GETTER(Float32, float32)
	DECLARE_INDIVIDUAL_GETTER(Float64, float64)
#undef DECLARE_INDIVIDUAL_GETTER
	String getString(const std::string & attribute_name, const id_t id) const override
	{
		const auto & attribute = getAttribute(attribute_name);
		if (attribute.type != AttributeType::string)
			throw Exception{
				"Type mismatch: attribute " + attribute_name + " has type " + toString(attribute.type),
				ErrorCodes::TYPE_MISMATCH
			};

		const auto & array = *std::get<std::unique_ptr<PODArray<StringRef>>>(attribute.arrays);

		return id < array.size() ? String{array[id]} : std::get<String>(attribute.null_values);
	}

#define DECLARE_MULTIPLE_GETTER(TYPE, LC_TYPE)\
	void get##TYPE(const std::string & attribute_name, const PODArray<id_t> & ids, PODArray<TYPE> & out) const override\
	{\
		const auto & attribute = getAttribute(attribute_name);\
		if (attribute.type != AttributeType::LC_TYPE)\
			throw Exception{\
				"Type mismatch: attribute " + attribute_name + " has type " + toString(attribute.type),\
				ErrorCodes::TYPE_MISMATCH\
			};\
		\
		getItems<TYPE>(attribute, ids, out);\
	}
	DECLARE_MULTIPLE_GETTER(UInt8, uint8)
	DECLARE_MULTIPLE_GETTER(UInt16, uint16)
	DECLARE_MULTIPLE_GETTER(UInt32, uint32)
	DECLARE_MULTIPLE_GETTER(UInt64, uint64)
	DECLARE_MULTIPLE_GETTER(Int8, int8)
	DECLARE_MULTIPLE_GETTER(Int16, int16)
	DECLARE_MULTIPLE_GETTER(Int32, int32)
	DECLARE_MULTIPLE_GETTER(Int64, int64)
	DECLARE_MULTIPLE_GETTER(Float32, float32)
	DECLARE_MULTIPLE_GETTER(Float64, float64)
#undef DECLARE_MULTIPLE_GETTER
	void getString(const std::string & attribute_name, const PODArray<id_t> & ids, ColumnString * out) const override
	{
		const auto & attribute = getAttribute(attribute_name);
		if (attribute.type != AttributeType::string)
			throw Exception{
				"Type mismatch: attribute " + attribute_name + " has type " + toString(attribute.type),
				ErrorCodes::TYPE_MISMATCH
			};

		const auto & attr = *std::get<std::unique_ptr<PODArray<StringRef>>>(attribute.arrays);
		const auto & null_value = std::get<String>(attribute.null_values);

		for (const auto i : ext::range(0, ids.size()))
		{
			const auto id = ids[i];
			const auto string_ref = id < attr.size() ? attr[id] : StringRef{null_value};
			out->insertData(string_ref.data, string_ref.size);
		}
	}

private:
	struct attribute_t final
	{
		AttributeType type;
		std::tuple<UInt8, UInt16, UInt32, UInt64,
			Int8, Int16, Int32, Int64,
			Float32, Float64,
			String> null_values;
		std::tuple<std::unique_ptr<PODArray<UInt8>>,
			std::unique_ptr<PODArray<UInt16>>,
			std::unique_ptr<PODArray<UInt32>>,
			std::unique_ptr<PODArray<UInt64>>,
			std::unique_ptr<PODArray<Int8>>,
			std::unique_ptr<PODArray<Int16>>,
			std::unique_ptr<PODArray<Int32>>,
			std::unique_ptr<PODArray<Int64>>,
			std::unique_ptr<PODArray<Float32>>,
			std::unique_ptr<PODArray<Float64>>,
			std::unique_ptr<PODArray<StringRef>>> arrays;
		std::unique_ptr<Arena> string_arena;
	};

	void createAttributes()
	{
		const auto size = dict_struct.attributes.size();
		attributes.reserve(size);

		for (const auto & attribute : dict_struct.attributes)
		{
			attribute_index_by_name.emplace(attribute.name, attributes.size());
			attributes.push_back(createAttributeWithType(getAttributeTypeByName(attribute.type),
				attribute.null_value));

			if (attribute.hierarchical)
			{
				hierarchical_attribute = &attributes.back();

				if (hierarchical_attribute->type != AttributeType::uint64)
					throw Exception{
						"Hierarchical attribute must be UInt64.",
						ErrorCodes::TYPE_MISMATCH
					};
			}
		}
	}

	void loadData()
	{
		auto stream = source_ptr->loadAll();
		stream->readPrefix();

		while (const auto block = stream->read())
		{
			const auto & id_column = *block.getByPosition(0).column;

			for (const auto attribute_idx : ext::range(0, attributes.size()))
			{
				const auto & attribute_column = *block.getByPosition(attribute_idx + 1).column;
				auto & attribute = attributes[attribute_idx];

				for (const auto row_idx : ext::range(0, id_column.size()))
					setAttributeValue(attribute, id_column[row_idx].get<UInt64>(), attribute_column[row_idx]);
			}
		}

		stream->readSuffix();
	}

	template <typename T>
	void createAttributeImpl(attribute_t & attribute, const std::string & null_value)
	{
		const auto & null_value_ref = std::get<T>(attribute.null_values) = DB::parse<T>(null_value);
		std::get<std::unique_ptr<PODArray<T>>>(attribute.arrays) =
			std::make_unique<PODArray<T>>(initial_array_size, null_value_ref);
	}

	attribute_t createAttributeWithType(const AttributeType type, const std::string & null_value)
	{
		attribute_t attr{type};

		switch (type)
		{
			case AttributeType::uint8: createAttributeImpl<UInt8>(attr, null_value); break;
			case AttributeType::uint16: createAttributeImpl<UInt16>(attr, null_value); break;
			case AttributeType::uint32: createAttributeImpl<UInt32>(attr, null_value); break;
			case AttributeType::uint64: createAttributeImpl<UInt64>(attr, null_value); break;
			case AttributeType::int8: createAttributeImpl<Int8>(attr, null_value); break;
			case AttributeType::int16: createAttributeImpl<Int16>(attr, null_value); break;
			case AttributeType::int32: createAttributeImpl<Int32>(attr, null_value); break;
			case AttributeType::int64: createAttributeImpl<Int64>(attr, null_value); break;
			case AttributeType::float32: createAttributeImpl<Float32>(attr, null_value); break;
			case AttributeType::float64: createAttributeImpl<Float64>(attr, null_value); break;
			case AttributeType::string:
			{
				const auto & null_value_ref = std::get<String>(attr.null_values) = DB::parse<String>(null_value);
				std::get<std::unique_ptr<PODArray<StringRef>>>(attr.arrays) =
					std::make_unique<PODArray<StringRef>>(initial_array_size, null_value_ref);
				attr.string_arena = std::make_unique<Arena>();
				break;
			}
		};

		return attr;
	}

	template <typename T>
	void getItems(const attribute_t & attribute, const PODArray<id_t> & ids, PODArray<T> & out) const
	{
		const auto & attr = *std::get<std::unique_ptr<PODArray<T>>>(attribute.arrays);
		const auto null_value = std::get<T>(attribute.null_values);

		for (const auto i : ext::range(0, ids.size()))
		{
			const auto id = ids[i];
			out[i] = id < attr.size() ? attr[id] : null_value;
		}
	}

	template <typename T>
	void setAttributeValueImpl(attribute_t & attribute, const id_t id, const T value)
	{
		auto & array = *std::get<std::unique_ptr<PODArray<T>>>(attribute.arrays);
		if (id >= array.size())
			array.resize_fill(id, std::get<T>(attribute.null_values));
		array[id] = value;
	}

	void setAttributeValue(attribute_t & attribute, const id_t id, const Field & value)
	{
		if (id >= max_array_size)
			throw Exception{
				"Identifier should be less than " + toString(max_array_size),
				ErrorCodes::ARGUMENT_OUT_OF_BOUND
			};

		switch (attribute.type)
		{
			case AttributeType::uint8: setAttributeValueImpl<UInt8>(attribute, id, value.get<UInt64>()); break;
			case AttributeType::uint16: setAttributeValueImpl<UInt16>(attribute, id, value.get<UInt64>()); break;
			case AttributeType::uint32: setAttributeValueImpl<UInt32>(attribute, id, value.get<UInt64>()); break;
			case AttributeType::uint64: setAttributeValueImpl<UInt64>(attribute, id, value.get<UInt64>()); break;
			case AttributeType::int8: setAttributeValueImpl<Int8>(attribute, id, value.get<Int64>()); break;
			case AttributeType::int16: setAttributeValueImpl<Int16>(attribute, id, value.get<Int64>()); break;
			case AttributeType::int32: setAttributeValueImpl<Int32>(attribute, id, value.get<Int64>()); break;
			case AttributeType::int64: setAttributeValueImpl<Int64>(attribute, id, value.get<Int64>()); break;
			case AttributeType::float32: setAttributeValueImpl<Float32>(attribute, id, value.get<Float64>()); break;
			case AttributeType::float64: setAttributeValueImpl<Float64>(attribute, id, value.get<Float64>()); break;
			case AttributeType::string:
			{
				auto & array = *std::get<std::unique_ptr<PODArray<StringRef>>>(attribute.arrays);
				if (id >= array.size())
					array.resize_fill(id, std::get<String>(attribute.null_values));
				const auto & string = value.get<String>();
				const auto string_in_arena = attribute.string_arena->insert(string.data(), string.size());
				array[id] = StringRef{string_in_arena, string.size()};
				break;
			}
		}
	}

	const attribute_t & getAttribute(const std::string & attribute_name) const
	{
		const auto it = attribute_index_by_name.find(attribute_name);
		if (it == std::end(attribute_index_by_name))
			throw Exception{
				"No such attribute '" + attribute_name + "'",
				ErrorCodes::BAD_ARGUMENTS
			};

		return attributes[it->second];
	}

	const std::string name;
	const DictionaryStructure dict_struct;
	const DictionarySourcePtr source_ptr;
	const DictionaryLifetime dict_lifetime;

	std::map<std::string, std::size_t> attribute_index_by_name;
	std::vector<attribute_t> attributes;
	const attribute_t * hierarchical_attribute = nullptr;
};

}