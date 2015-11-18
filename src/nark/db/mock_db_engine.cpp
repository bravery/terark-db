#include "mock_db_engine.hpp"
#include <nark/io/FileStream.hpp>
#include <nark/io/StreamBuffer.hpp>
#include <nark/io/DataIO.hpp>
#include <boost/filesystem.hpp>

namespace nark {

namespace fs = boost::filesystem;

llong MockReadonlyStore::dataStorageSize() const {
	return m_rows.used_mem_size();
}
llong MockReadonlyStore::numDataRows() const {
	return m_rows.size();
}
void
MockReadonlyStore::getValue(llong id, valvec<byte>* val, BaseContextPtr&)
const {
	assert(id >= 0);
	assert(id < llong(m_rows.size()));
	val->assign(m_rows[id]);
}
StoreIteratorPtr MockReadonlyStore::createStoreIter() const {
	return nullptr;
}
BaseContextPtr MockReadonlyStore::createStoreContext() const {
	return nullptr;
}

void MockReadonlyStore::save(fstring path1) const {
	fs::path fpath = path1.c_str();
	FileStream fp(fpath.string().c_str(), "wb");
	fp.disbuf();
	NativeDataOutput<OutputBuffer> dio; dio.attach(&fp);
	dio << m_rows;
}
void MockReadonlyStore::load(fstring path1) {
	fs::path fpath = path1.c_str();
	FileStream fp(fpath.string().c_str(), "rb");
	fp.disbuf();
	NativeDataInput<InputBuffer> dio; dio.attach(&fp);
	dio >> m_rows;
}

struct FixedLenKeyCompare {
	bool operator()(size_t x, fstring y) const {
		fstring xs(strpool + fixedLen * x, fixedLen);
		return schema->compareData(xs, y) < 0;
	}
	bool operator()(fstring x, size_t y) const {
		return (*this)(y, x);
	}
	size_t fixedLen;
	const char  * strpool;
	const Schema* schema;
};

struct VarLenKeyCompare {
	bool operator()(size_t x, fstring y) const {
		size_t xoff0 = offsets[x], xoff1 = offsets[x+1];
		fstring xs(strpool + xoff0, xoff1 - xoff0);
		return schema->compareData(xs, y) < 0;
	}
	bool operator()(fstring x, size_t y) const {
		return (*this)(y, x);
	}
	const char    * strpool;
	const uint32_t* offsets;
	const Schema  * schema;
};

class MockReadonlyIndexIterator : public IndexIterator {
	friend class MockReadonlyIndex;
	size_t m_pos = size_t(-1);

public:
	bool increment() override {
		auto owner = static_cast<const MockReadonlyIndex*>(m_index);
		if (size_t(-1) == m_pos) {
			m_pos = 0;
		}
		if (m_pos < owner->m_keys.size()) {
			m_pos++;
			return true;
		}
		return false;
	}
	bool decrement() override {
		auto owner = static_cast<const MockReadonlyIndex*>(m_index);
		if (size_t(-1) == m_pos) {
			m_pos = owner->m_keys.size() - 1;
			return true;
		}
		if (m_pos > 0) {
			m_pos--;
			return true;
		}
		return false;
	}
	void reset() {
		m_pos = size_t(-1);
	}
	bool seekExact(fstring key) override {
		size_t lo;
		if (seekLowerBound_imp(key, &lo)) {
			m_pos = lo;
			return true;
		}
		return false;
	}
	bool seekLowerBound(fstring key) override {
		return seekLowerBound_imp(key, &m_pos);
	}
	bool seekLowerBound_imp(fstring key, size_t* lowerBound) {
		auto owner = static_cast<const MockReadonlyIndex*>(m_index);
		const uint32_t* index = owner->m_fix.data();
		const size_t rows = owner->m_fix.size();
		const size_t fixlen = owner->m_fixedLen;
		if (fixlen) {
			FixedLenKeyCompare cmp;
			cmp.fixedLen = fixlen;
			cmp.strpool = owner->m_keys.strpool.data();
			cmp.schema = owner->m_schema.get();
			size_t lo = nark::lower_bound_0(index, rows, key, cmp);
			*lowerBound = lo;
			if (lo < rows && key == owner->m_keys[lo]) {
				return true;
			}
		}
		else {
			VarLenKeyCompare cmp;
			cmp.offsets = owner->m_keys.offsets.data();
			cmp.strpool = owner->m_keys.strpool.data();
			cmp.schema = owner->m_schema.get();
			size_t lo = nark::lower_bound_0(index, rows, key, cmp);
			*lowerBound = lo;
			if (lo < rows && key == owner->m_keys[lo]) {
				return true;
			}
		}
		return false;
	}

	void getIndexKey(llong* id, valvec<byte>* key) const override {
		auto owner = static_cast<const MockReadonlyIndex*>(m_index);
		assert(m_pos < owner->m_fix.size());
		*id = m_pos;
		fstring k = owner->m_keys[m_pos];
		key->assign(k.udata(), k.size());
	}
};

MockReadonlyIndex::MockReadonlyIndex(SchemaPtr schema) {
	m_schema = schema;
}

MockReadonlyIndex::~MockReadonlyIndex() {
}

StoreIteratorPtr MockReadonlyIndex::createStoreIter() const {
	assert(!"Readonly column store did not define iterator");
	return nullptr;
}

BaseContextPtr MockReadonlyIndex::createIndexContext() const {
	return nullptr;
}

BaseContextPtr MockReadonlyIndex::createStoreContext() const {
	return nullptr;
}

#ifdef _MSC_VER
#define qsort_r qsort_s
#endif

void
MockReadonlyIndex::build(SortableStrVec& keys) {
	const Schema* pSchema = m_schema.get();
	size_t fixlen = pSchema->getFixedRowLen();
	const byte* base = keys.m_strpool.data();
	if (fixlen) {
		assert(keys.m_index.size() == 0);
		m_fix.resize_no_init(keys.size() / fixlen);
		for (size_t i = 0; i < m_fix.size(); ++i) m_fix[i] = i;
		std::sort(m_fix.begin(), m_fix.end(), [=](size_t x, size_t y) {
			fstring xs(base + fixlen * x, fixlen);
			fstring ys(base + fixlen * y, fixlen);
			return pSchema->compareData(xs, ys);
		});
	}
	else {
		if (keys.str_size() >= UINT32_MAX) {
			THROW_STD(length_error,
				"keys.str_size=%lld is too large", llong(keys.str_size()));
		}
		// reuse memory of keys.m_index
		auto offsets = (uint32_t*)keys.m_index.data();
		size_t rows = keys.m_index.size();
		m_fix.resize_no_init(rows);
		for (size_t i = 0; i < rows; ++i) m_fix[i] = i;
		for (size_t i = 0; i < rows; ++i) {
			uint32_t offset = uint32_t(keys.m_index[i].offset);
			offsets[i] = offset;
		}
		offsets[rows] = keys.str_size();
		std::sort(m_fix.begin(), m_fix.end(), [=](size_t x, size_t y) {
			size_t xoff0 = offsets[x], xoff1 = offsets[x+1];
			size_t yoff0 = offsets[y], yoff1 = offsets[y+1];
			fstring xs(base + xoff0, xoff1 - xoff0);
			fstring ys(base + yoff0, yoff1 - yoff0);
			return pSchema->compareData(xs, ys);
		});
		BOOST_STATIC_ASSERT(sizeof(SortableStrVec::SEntry) == 4*3);
		m_keys.offsets.risk_set_data(offsets);
		m_keys.offsets.risk_set_size(rows + 1);
		m_keys.offsets.risk_set_capacity(3 * rows);
		m_keys.shrink_to_fit();
		keys.m_index.risk_release_ownership();
	}
	m_keys.strpool.swap((valvec<char>&)keys.m_strpool);
	m_fixedLen = fixlen;
}

void MockReadonlyIndex::save(fstring path1) const {
	fs::path fpath = path1.c_str();
	FileStream fp(fpath.string().c_str(), "wb");
	fp.disbuf();
	NativeDataOutput<OutputBuffer> dio; dio.attach(&fp);
	size_t rows = m_fix.size();
	dio << uint64_t(m_fixedLen);
	dio << uint64_t(rows);
	dio << uint64_t(m_keys.size());
	dio.ensureWrite(m_fix.data(), m_fix.used_mem_size());
	if (m_fixedLen) {
		assert(m_fix.size() != 0);
		assert(m_keys.size() == 0);
	} else {
		assert(m_keys.size() != 0);
		dio.ensureWrite(m_keys.offsets.data(), m_keys.offsets.used_mem_size());
	}
	dio.ensureWrite(m_keys.strpool.data(), m_keys.strpool.used_mem_size());
}

void MockReadonlyIndex::load(fstring path1) {
	fs::path fpath = path1.c_str();
	FileStream fp(fpath.string().c_str(), "rb");
	fp.disbuf();
	NativeDataInput<InputBuffer> dio; dio.attach(&fp);
	uint64_t fixlen, rows, keylen;
	dio >> fixlen;
	dio >> rows;
	dio >> keylen;
	m_fix.resize_no_init(size_t(rows));
	dio.ensureRead(m_fix.data(), m_fix.used_mem_size());
	if (0 == fixlen) {
		m_keys.offsets.resize_no_init(size_t(rows + 1));
		dio.ensureRead(m_keys.offsets.data(), m_keys.offsets.used_mem_size());
	}
	m_keys.strpool.resize_no_init(size_t(keylen));
	dio.ensureRead(m_keys.strpool.data(), size_t(keylen));
	m_fixedLen = size_t(fixlen);
}

llong MockReadonlyIndex::numDataRows() const {
	return m_fix.size();
}
llong MockReadonlyIndex::dataStorageSize() const {
	return m_fix.used_mem_size()
		+ m_keys.offsets.used_mem_size()
		+ m_keys.strpool.used_mem_size();
}

void MockReadonlyIndex::getValue(llong id, valvec<byte>* key, BaseContextPtr&) const {
	assert(m_idToKey.size() == m_keyVec.size());
	assert(id < (llong)m_idToKey.size());
	assert(id >= 0);
	if (m_fixedLen) {
		fstring key1(m_keys.strpool.data() + m_fixedLen * id, m_fixedLen);
		key->assign(key1.udata(), key1.size());
	}
	else {
		size_t idx = m_fix[id];
		fstring key1 = m_keys[idx];
		key->assign(key1.udata(), key1.size());
	}
}

IndexIteratorPtr MockReadonlyIndex::createIndexIter() const {
	return new MockReadonlyIndexIterator();
}

llong MockReadonlyIndex::numIndexRows() const {
	return m_fix.size();
}

llong MockReadonlyIndex::indexStorageSize() const {
	return m_fix.used_mem_size() + m_keys.offsets.used_mem_size();
}

//////////////////////////////////////////////////////////////////
template<class WrStore>
class MockWritableStoreIter : public StoreIterator {
	ptrdiff_t m_id;
public:
	MockWritableStoreIter(const WrStore* store) {
		m_store.reset(const_cast<WrStore*>(store));
		m_id = -1;
	}
	bool increment() override {
		auto store = static_cast<WrStore*>(m_store.get());
		m_id++;
		return m_id < ptrdiff_t(store->m_rows.size());
	}
	void getKeyVal(llong* idKey, valvec<byte>* val) const override {
		auto store = static_cast<WrStore*>(m_store.get());
		assert(m_id < ptrdiff_t(store->m_rows.size()));
		*idKey = m_id;
		*val = store->m_rows[m_id];
	}
};

void MockWritableStore::save(fstring path1) const {
	fs::path fpath = path1.c_str();
	FileStream fp(fpath.string().c_str(), "wb");
	fp.disbuf();
	NativeDataOutput<OutputBuffer> dio; dio.attach(&fp);
	dio << m_rows;
}
void MockWritableStore::load(fstring path1) {
	fs::path fpath = path1.c_str();
	FileStream fp(fpath.string().c_str(), "rb");
	fp.disbuf();
	NativeDataInput<InputBuffer> dio; dio.attach(&fp);
	dio >> m_rows;
}

llong MockWritableStore::dataStorageSize() const {
	return m_rows.used_mem_size() + m_dataSize;
}

llong MockWritableStore::numDataRows() const {
	return m_rows.size();
}

void MockWritableStore::getValue(llong id, valvec<byte>* val, BaseContextPtr&) const {
	assert(id >= 0);
	assert(id < llong(m_rows.size()));
	*val = m_rows[id];
}

StoreIteratorPtr MockWritableStore::createStoreIter() const {
	return new MockWritableStoreIter<MockWritableStore>(this);
}

BaseContextPtr MockWritableStore::createStoreContext() const {
	return nullptr;
}

llong MockWritableStore::append(fstring row, BaseContextPtr&) {
	llong id = m_rows.size();
	m_rows.push_back();
	m_rows.back().assign(row);
	return id;
}
void MockWritableStore::replace(llong id, fstring row, BaseContextPtr&) {
	assert(id >= 0);
	assert(id < llong(m_rows.size()));
	m_rows[id].assign(row);
}
void MockWritableStore::remove(llong id, BaseContextPtr&) {
	assert(id >= 0);
	assert(id < llong(m_rows.size()));
	m_rows[id].clear();
}

//////////////////////////////////////////////////////////////////

IndexIteratorPtr MockWritableIndex::createIndexIter() const {
	return new MockReadonlyIndexIterator();
}

void MockWritableIndex::save(fstring path1) const {
	fs::path fpath = path1.c_str();
	FileStream fp(fpath.string().c_str(), "wb");
	fp.disbuf();
	NativeDataOutput<OutputBuffer> dio; dio.attach(&fp);
	dio << m_kv;
}
void MockWritableIndex::load(fstring path1) {
	fs::path fpath = path1.c_str();
	FileStream fp(fpath.string().c_str(), "rb");
	fp.disbuf();
	NativeDataInput<InputBuffer> dio; dio.attach(&fp);
	dio >> m_kv;
}

llong MockWritableIndex::numIndexRows() const {
	return m_kv.size();
}

llong MockWritableIndex::indexStorageSize() const {
	// std::set's rbtree node needs 4ptr space
	return m_kv.size() * (sizeof(kv_t) + 4 * sizeof(void*));
}

size_t MockWritableIndex::insert(fstring key, llong id, BaseContextPtr&) {
	auto ib = m_kv.insert(std::make_pair(key.str(), id));
	return ib.second;
}
size_t MockWritableIndex::replace(fstring key, llong oldId, llong newId, BaseContextPtr&) {
	if (oldId != newId) {
		m_kv.erase(std::make_pair(key.str(), oldId));
	}
	auto ib = m_kv.insert(std::make_pair(key.str(), newId));
	return ib.second;
}
size_t MockWritableIndex::remove(fstring key, BaseContextPtr&) {
	std::string key1 = key.str();
	auto iter = m_kv.lower_bound(std::make_pair(key1, 0));
	size_t cnt = 0;
	while (iter != m_kv.end() && iter->first == key1) {
		auto next = iter; ++next;
		m_kv.erase(iter);
		iter = next;
		cnt++;
	}
	return cnt;
}
size_t MockWritableIndex::remove(fstring key, llong id, BaseContextPtr&) {
	return m_kv.erase(std::make_pair(key.str(), id));
}
void MockWritableIndex::flush() {
	// do nothing
}

///////////////////////////////////////////////////////////////////////
MockReadonlySegment::MockReadonlySegment() {
}
MockReadonlySegment::~MockReadonlySegment() {
}

ReadableStorePtr
MockReadonlySegment::openPart(fstring path) const {
	// Mock just use one kind of data store
//	FileStream fp(path.c_str(), "rb");
//	fp.disbuf();
//	NativeDataInput<InputBuffer> dio; dio.attach(&fp);
	ReadableStorePtr store(new MockReadonlyStore());
	store->load(path);
	return store;
}

ReadableStoreIndexPtr
MockReadonlySegment::openIndex(fstring path, SchemaPtr schema) const {
	ReadableStoreIndexPtr store(new MockReadonlyIndex(schema));
	store->load(path);
	return store;
}

ReadableStoreIndexPtr
MockReadonlySegment::buildIndex(SchemaPtr indexSchema,
								SortableStrVec& indexData)
const {
	std::unique_ptr<MockReadonlyIndex> index(new MockReadonlyIndex(indexSchema));
	index->build(indexData);
	return index.release();
}

ReadableStorePtr
MockReadonlySegment::buildStore(SortableStrVec& storeData) const {
	std::unique_ptr<MockReadonlyStore> store(new MockReadonlyStore());
	return store.release();
}

///////////////////////////////////////////////////////////////////////////
MockWritableSegment::MockWritableSegment() {
}
MockWritableSegment::~MockWritableSegment() {
}

void MockWritableSegment::save(fstring path1) const {
	fs::path fpath = path1.c_str();
	FileStream fp(fpath.string().c_str(), "wb");
	fp.disbuf();
	NativeDataOutput<OutputBuffer> dio; dio.attach(&fp);
	dio << m_rows;
}

void MockWritableSegment::load(fstring path1) {
	fs::path fpath = path1.c_str();
	FileStream fp(fpath.string().c_str(), "rb");
	fp.disbuf();
	NativeDataInput<InputBuffer> dio; dio.attach(&fp);
	dio >> m_rows;
}

llong MockWritableSegment::dataStorageSize() const {
	return m_rows.used_mem_size() + m_dataSize;
}

void
MockWritableSegment::getValue(llong id, valvec<byte>* val,
							  BaseContextPtr&)
const {
	assert(id >= 0);
	assert(id < llong(m_rows.size()));
	*val = m_rows[id];
}

StoreIteratorPtr MockWritableSegment::createStoreIter() const {
	return StoreIteratorPtr(new MockWritableStoreIter<MockWritableSegment>(this));
}

BaseContextPtr MockWritableSegment::createStoreContext() const {
	return nullptr;
}

llong MockWritableSegment::totalStorageSize() const {
	return totalIndexSize() + m_rows.used_mem_size() + m_dataSize;
}

llong MockWritableSegment::append(fstring row, BaseContextPtr &) {
	llong id = m_rows.size();
	m_rows.push_back();
	m_rows.back().assign(row);
	return id;
}

void MockWritableSegment::replace(llong id, fstring row, BaseContextPtr &) {
	assert(id >= 0);
	assert(id < llong(m_rows.size()));
	m_rows[id].assign(row);
}

void MockWritableSegment::remove(llong id, BaseContextPtr &) {
	assert(id >= 0);
	assert(id < llong(m_rows.size()));
	m_rows[id].clear();
}

void MockWritableSegment::flush() {
	// do nothing
}

///////////////////////////////////////////////////////////////////////////

ReadonlySegmentPtr
MockCompositeTable::createReadonlySegment() const {
	return new MockReadonlySegment();
}
WritableSegmentPtr
MockCompositeTable::createWritableSegment(fstring dir) const {
	return new MockWritableSegment();
}

WritableSegmentPtr
MockCompositeTable::openWritableSegment(fstring dir) const {
	WritableSegmentPtr seg(new MockWritableSegment());
	seg->load(dir);
	return seg;
}

} // namespace nark
