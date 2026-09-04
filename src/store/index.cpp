#include "index.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <string>
#include <system_error>

#include <boost/capy/buffers.hpp>
#include <boost/capy/ex/this_coro.hpp>

namespace liquid::store_detail {
namespace {
constexpr std::array<std::byte, 8> run_magic{std::byte{'L'},std::byte{'Q'},std::byte{'I'},std::byte{'D'},std::byte{'X'},std::byte{1},std::byte{0},std::byte{0}};
constexpr std::array<std::byte, 8> manifest_magic{std::byte{'L'},std::byte{'Q'},std::byte{'M'},std::byte{'A'},std::byte{'N'},std::byte{1},std::byte{0},std::byte{0}};
constexpr std::size_t record_size = 48, fanout_count = 257, run_header_size = 8 + 1 + 7 + 8 + 8 + fanout_count * 8 + 8;
constexpr std::size_t lookup_slice_limit = 64 * 1024, window_size = 1024 * 1024;
error failure(errc code, std::string text, const std::filesystem::path& path) { return {code, std::move(text), path.string()}; }
void put32(std::vector<std::byte>& out, std::uint32_t value) { for (unsigned n=0;n<4;++n) out.push_back(std::byte{static_cast<unsigned char>(value>>(n*8U))}); }
void put64(std::vector<std::byte>& out, std::uint64_t value) { for (unsigned n=0;n<8;++n) out.push_back(std::byte{static_cast<unsigned char>(value>>(n*8U))}); }
std::uint32_t get32(std::span<const std::byte> in, std::size_t at) { std::uint32_t out=0; for(unsigned n=0;n<4 && at+n<in.size();++n) out|=std::uint32_t(std::to_integer<unsigned char>(in[at+n]))<<(n*8U); return out; }
std::uint64_t get64(std::span<const std::byte> in, std::size_t at) { std::uint64_t out=0; for(unsigned n=0;n<8 && at+n<in.size();++n) out|=std::uint64_t(std::to_integer<unsigned char>(in[at+n]))<<(n*8U); return out; }
std::uint64_t check(std::span<const std::byte> in) { std::uint64_t out=1469598103934665603ULL; for(auto b:in){out^=std::to_integer<unsigned char>(b);out*=1099511628211ULL;} return out; }
bool valid(unsigned char value) { return value==static_cast<unsigned char>(algorithm::blake3)||value==static_cast<unsigned char>(algorithm::sha256); }
std::uint64_t hash_bits(const std::array<std::byte,32>& id) { return check(id); }
void bloom_set(std::vector<std::byte>& bloom, const std::array<std::byte,32>& id) { const auto bits=bloom.size()*8; const auto h=hash_bits(id); for(unsigned n=0;n<7;++n){const auto bit=(h+n*0x9e3779b97f4a7c15ULL)%bits;bloom[bit/8]|=std::byte{static_cast<unsigned char>(1U<<(bit%8))};} }
bool bloom_has(std::span<const std::byte> bloom, const digest& id) { if(bloom.empty()) return false; const auto bits=bloom.size()*8; const auto h=hash_bits(id.bytes); for(unsigned n=0;n<7;++n){const auto bit=(h+n*0x9e3779b97f4a7c15ULL)%bits;if(!(std::to_integer<unsigned char>(bloom[bit/8])&(1U<<(bit%8))))return false;}return true; }
index_entry decode(std::span<const std::byte> in, algorithm alg) { index_entry out{};out.id.alg=alg;std::copy_n(in.begin(),32,out.id.bytes.begin());out.location={get32(in,32),get64(in,36),get32(in,44)};return out; }
void encode(std::vector<std::byte>& out,const std::array<std::byte,32>& id,chunk_location value){out.insert(out.end(),id.begin(),id.end());put32(out,value.pack_id);put64(out,value.offset);put32(out,value.length);}
std::vector<std::byte> header(algorithm alg,std::uint64_t count,std::uint64_t bloom,const std::array<std::uint64_t,fanout_count>& fanout){std::vector<std::byte> out;out.reserve(run_header_size);out.insert(out.end(),run_magic.begin(),run_magic.end());out.push_back(std::byte{static_cast<unsigned char>(alg)});out.insert(out.end(),7,std::byte{0});put64(out,count);put64(out,bloom);for(auto value:fanout)put64(out,value);put64(out,check(out));return out;}
unsigned size_tier(std::uint64_t value) { unsigned tier=0;while(value>1){value>>=1;++tier;}return tier; }
}

chunk_index::chunk_index(std::filesystem::path root, chunk_index_options options) : root_(std::move(root)),options_(options) { options_.memtable_entries=std::max<std::size_t>(1,options_.memtable_entries);options_.compaction_runs=std::max<std::size_t>(1,options_.compaction_runs); }

runtime::task<void> chunk_index::open() {
 try {
  states_.clear(); next_sequence_=1; std::filesystem::create_directories(root_); const auto manifest=root_/"MANIFEST"; if(!std::filesystem::exists(manifest)) co_return result<void>{};
  auto raw=co_await read_file(manifest);if(!raw)co_return std::unexpected(raw.error());auto bytes=std::span<const std::byte>(*raw);
  if(bytes.size()<24||!std::equal(manifest_magic.begin(),manifest_magic.end(),bytes.begin()))co_return std::unexpected(failure(errc::corrupt,"invalid index manifest",manifest));
  const auto count=get32(bytes,8);highest_pack_id_=get32(bytes,12);
  if(bytes.size()!=16+std::size_t(count)*24+8||check(bytes.first(bytes.size()-8))!=get64(bytes,bytes.size()-8))co_return std::unexpected(failure(errc::corrupt,"corrupt index manifest",manifest));
  for(std::size_t n=0,at=16;n<count;++n,at+=24){
   const auto alg_byte=std::to_integer<unsigned char>(bytes[at]);const auto sequence=get64(bytes,at+8), records=get64(bytes,at+16);const auto path=root_/("run-"+std::to_string(sequence)+".idx");
   if(!valid(alg_byte))co_return std::unexpected(failure(errc::corrupt,"unknown run algorithm",manifest));
   auto raw_header=co_await read_file_range(path,0,run_header_size);if(!raw_header)co_return std::unexpected(raw_header.error());auto head=std::span<const std::byte>(*raw_header);
   if(!std::equal(run_magic.begin(),run_magic.end(),head.begin())||std::to_integer<unsigned char>(head[8])!=alg_byte||get64(head,16)!=records||check(head.first(run_header_size-8))!=get64(head,run_header_size-8))co_return std::unexpected(failure(errc::corrupt,"corrupt index run header",path));
   const auto bloom_size=get64(head,24);
   if(bloom_size>std::numeric_limits<std::size_t>::max()||std::filesystem::file_size(path)!=run_header_size+bloom_size+records*record_size)co_return std::unexpected(failure(errc::corrupt,"invalid index run size",path));
   run loaded{};loaded.sequence=sequence;loaded.algorithm_value=static_cast<algorithm>(alg_byte);loaded.path=path;loaded.record_count=records;
   for(std::size_t i=0;i<fanout_count;++i)loaded.fanout[i]=get64(head,32+i*8);
   if(loaded.fanout.front()!=0||loaded.fanout.back()!=records||!std::is_sorted(loaded.fanout.begin(),loaded.fanout.end()))co_return std::unexpected(failure(errc::corrupt,"invalid index fanout",path));
   auto bloom=co_await read_file_range(path,run_header_size,static_cast<std::size_t>(bloom_size));if(!bloom)co_return std::unexpected(bloom.error());loaded.bloom=std::move(*bloom);
   states_[loaded.algorithm_value].runs.push_back(std::move(loaded));next_sequence_=std::max(next_sequence_,sequence+1);
  }
  for(auto& [ignored,state]:states_)std::sort(state.runs.begin(),state.runs.end(),[](const run& a,const run& b){return a.sequence>b.sequence;});co_return result<void>{};
 } catch(const std::exception& e){co_return std::unexpected(failure(errc::io,e.what(),root_));}
}

runtime::task<std::vector<std::byte>> chunk_index::read_run_range(run& source,std::uint64_t offset,std::size_t size) {
 try {
  const auto executor=co_await boost::capy::this_coro::executor;
  if(!source.file){source.file=std::make_unique<boost::corosio::random_access_file>(executor.context());source.file->open(source.path,boost::corosio::file_base::read_only);}
  std::vector<std::byte> out(size);std::size_t read=0;while(read<out.size()){auto [ec,count]=co_await source.file->read_some_at(offset+read,boost::capy::mutable_buffer(out.data()+read,out.size()-read));if(ec||count==0)co_return std::unexpected(failure(errc::io,ec?ec.message():"short index read",source.path));read+=count;note_file_read(count);}co_return out;
 }catch(const std::system_error& e){co_return std::unexpected(failure(errc::io,e.what(),source.path));}
}

runtime::task<std::optional<chunk_location>> chunk_index::find_in_run(run& source,digest id) {
 if(!bloom_has(source.bloom,id))co_return std::nullopt;const auto bucket=std::to_integer<unsigned char>(id.bytes[0]);const auto first=source.fanout[bucket],last=source.fanout[bucket+1];if(first==last)co_return std::nullopt;
 const auto offset=run_header_size+source.bloom.size()+first*record_size, bytes=(last-first)*record_size;
 if(bytes<=lookup_slice_limit){auto data=co_await read_run_range(source,offset,static_cast<std::size_t>(bytes));if(!data)co_return std::unexpected(data.error());std::size_t low=0,high=last-first;while(low<high){const auto mid=low+(high-low)/2;const auto record=std::span<const std::byte>(*data).subspan(mid*record_size,record_size);if(std::lexicographical_compare(record.begin(),record.begin()+32,id.bytes.begin(),id.bytes.end()))low=mid+1;else high=mid;}if(low==last-first)co_return std::nullopt;const auto record=std::span<const std::byte>(*data).subspan(low*record_size,record_size);if(!std::equal(record.begin(),record.begin()+32,id.bytes.begin()))co_return std::nullopt;co_return decode(record,id.alg).location;}
 std::uint64_t low=first,high=last;while(low<high){const auto mid=low+(high-low)/2;auto data=co_await read_run_range(source,run_header_size+source.bloom.size()+mid*record_size,record_size);if(!data)co_return std::unexpected(data.error());if(std::lexicographical_compare(data->begin(),data->begin()+32,id.bytes.begin(),id.bytes.end()))low=mid+1;else high=mid;}if(low==last)co_return std::nullopt;auto data=co_await read_run_range(source,run_header_size+source.bloom.size()+low*record_size,record_size);if(!data)co_return std::unexpected(data.error());if(!std::equal(data->begin(),data->begin()+32,id.bytes.begin()))co_return std::nullopt;co_return decode(*data,id.alg).location;
}

runtime::task<void> chunk_index::insert(index_entry entry) {
 auto& table=states_[entry.id.alg].memtable;if(!table.contains(entry.id.bytes)){auto present=co_await find(entry.id);if(!present)co_return std::unexpected(present.error());if(!*present)table.emplace(entry.id.bytes,entry.location);}if(table.size()>=options_.memtable_entries)co_return co_await flush(entry.id.alg);co_return result<void>{};
}

runtime::task<void> chunk_index::flush(algorithm alg) {
 auto state_it=states_.find(alg);if(state_it==states_.end()||state_it->second.memtable.empty())co_return result<void>{};auto& state=state_it->second;const auto count=static_cast<std::uint64_t>(state.memtable.size()), bloom_size=std::max<std::uint64_t>(1,(count*10+7)/8);std::array<std::uint64_t,fanout_count> fanout{};std::vector<std::byte> bloom(static_cast<std::size_t>(bloom_size));
 for(const auto& [key,value]:state.memtable){++fanout[std::to_integer<unsigned char>(key[0])+1];bloom_set(bloom,key);}for(std::size_t i=1;i<fanout.size();++i)fanout[i]+=fanout[i-1];
 const auto sequence=next_sequence_++;const auto path=root_/("run-"+std::to_string(sequence)+".idx");auto created=co_await write_file(path,header(alg,count,bloom_size,fanout));if(!created)co_return std::unexpected(created.error());auto written=co_await write_file_range(path,run_header_size,bloom);if(!written)co_return std::unexpected(written.error());
 std::vector<std::byte> buffer;buffer.reserve(window_size);std::uint64_t at=run_header_size+bloom_size;for(const auto& [key,value]:state.memtable){encode(buffer,key,value);if(buffer.size()>=window_size){const auto size=buffer.size();auto saved=co_await write_file_range(path,at,std::move(buffer));if(!saved)co_return std::unexpected(saved.error());at+=size;buffer.clear();buffer.reserve(window_size);}}if(!buffer.empty()){auto saved=co_await write_file_range(path,at,std::move(buffer));if(!saved)co_return std::unexpected(saved.error());}
 state.memtable.clear();run added{};added.sequence=sequence;added.algorithm_value=alg;added.path=path;added.record_count=count;added.fanout=fanout;added.bloom=std::move(bloom);state.runs.insert(state.runs.begin(),std::move(added));auto merged=co_await compact(alg);if(!merged)co_return std::unexpected(merged.error());co_return co_await write_manifest();
}

runtime::task<void> chunk_index::flush(){for(const auto& [alg,ignored]:states_){auto saved=co_await flush(alg);if(!saved)co_return std::unexpected(saved.error());}co_return co_await write_manifest();}
runtime::task<std::optional<chunk_location>> chunk_index::find(digest id){auto state=states_.find(id.alg);if(state==states_.end())co_return std::nullopt;if(const auto found=state->second.memtable.find(id.bytes);found!=state->second.memtable.end())co_return found->second;std::optional<chunk_location> answer;std::optional<error> problem;for(auto& source:state->second.runs){auto found=co_await find_in_run(source,id);if(!found){problem=found.error();break;}if(*found){answer=*found;break;}}// block_on creates a short-lived executor for public calls, so release a run file before that executor is destroyed.
 for(auto& source:state->second.runs)if(source.file){source.file->close();source.file.reset();}if(problem)co_return std::unexpected(*problem);co_return answer;}

runtime::task<void> chunk_index::compact(algorithm alg) {
 auto& state=states_[alg];std::map<unsigned,std::vector<std::size_t>> groups;for(std::size_t i=0;i<state.runs.size();++i)groups[size_tier(state.runs[i].record_count)].push_back(i);auto group=std::find_if(groups.begin(),groups.end(),[this](const auto& item){return item.second.size()>options_.compaction_runs;});if(group==groups.end())co_return result<void>{};
 std::vector<run*> input;for(auto index:group->second)input.push_back(&state.runs[index]);struct cursor{run* source;std::uint64_t pos{},begin{};std::vector<std::byte> window;};std::vector<cursor> cursors;for(auto* source:input)cursors.push_back({source});
 const auto next=[this,alg](cursor& cursor)->runtime::task<std::optional<index_entry>>{if(cursor.pos==cursor.source->record_count)co_return std::nullopt;if(cursor.window.empty()||cursor.pos<cursor.begin||cursor.pos>=cursor.begin+cursor.window.size()/record_size){cursor.begin=cursor.pos;const auto records=std::min<std::uint64_t>(cursor.source->record_count-cursor.pos,window_size/record_size);auto bytes=co_await read_run_range(*cursor.source,run_header_size+cursor.source->bloom.size()+cursor.pos*record_size,static_cast<std::size_t>(records*record_size));if(!bytes)co_return std::unexpected(bytes.error());cursor.window=std::move(*bytes);}const auto offset=static_cast<std::size_t>((cursor.pos-cursor.begin)*record_size);++cursor.pos;co_return decode(std::span<const std::byte>(cursor.window).subspan(offset,record_size),alg);};
 std::vector<std::optional<index_entry>> heads(cursors.size());std::uint64_t capacity=0;for(std::size_t i=0;i<cursors.size();++i){capacity+=input[i]->record_count;auto entry=co_await next(cursors[i]);if(!entry)co_return std::unexpected(entry.error());heads[i]=std::move(*entry);}
 const auto bloom_size=std::max<std::uint64_t>(1,(capacity*10+7)/8),sequence=next_sequence_++;const auto path=root_/("run-"+std::to_string(sequence)+".idx");std::array<std::uint64_t,fanout_count> fanout{};std::vector<std::byte> bloom(static_cast<std::size_t>(bloom_size));auto created=co_await write_file(path,header(alg,0,bloom_size,fanout));if(!created)co_return std::unexpected(created.error());auto blank=co_await write_file_range(path,run_header_size,bloom);if(!blank)co_return std::unexpected(blank.error());
 std::vector<std::byte> buffer;buffer.reserve(window_size);std::uint64_t at=run_header_size+bloom_size,count=0;while(true){std::optional<std::size_t> choice;for(std::size_t i=0;i<heads.size();++i)if(heads[i]&&(!choice||heads[i]->id.bytes<heads[*choice]->id.bytes))choice=i;if(!choice)break;const auto key=heads[*choice]->id.bytes;std::size_t newest=*choice;for(std::size_t i=0;i<heads.size();++i)if(heads[i]&&heads[i]->id.bytes==key&&input[i]->sequence>input[newest]->sequence)newest=i;encode(buffer,key,heads[newest]->location);bloom_set(bloom,key);++fanout[std::to_integer<unsigned char>(key[0])+1];++count;if(buffer.size()>=window_size){const auto size=buffer.size();auto saved=co_await write_file_range(path,at,std::move(buffer));if(!saved)co_return std::unexpected(saved.error());at+=size;buffer.clear();buffer.reserve(window_size);}for(std::size_t i=0;i<heads.size();++i)if(heads[i]&&heads[i]->id.bytes==key){auto entry=co_await next(cursors[i]);if(!entry)co_return std::unexpected(entry.error());heads[i]=std::move(*entry);}}
 if(!buffer.empty()){auto saved=co_await write_file_range(path,at,std::move(buffer));if(!saved)co_return std::unexpected(saved.error());}for(std::size_t i=1;i<fanout.size();++i)fanout[i]+=fanout[i-1];auto final_header=co_await write_file_range(path,0,header(alg,count,bloom_size,fanout));if(!final_header)co_return std::unexpected(final_header.error());auto final_bloom=co_await write_file_range(path,run_header_size,bloom);if(!final_bloom)co_return std::unexpected(final_bloom.error());
 std::vector<run> kept;kept.reserve(state.runs.size()-input.size()+1);for(auto& source:state.runs)if(std::find(input.begin(),input.end(),&source)==input.end())kept.push_back(std::move(source));run merged{};merged.sequence=sequence;merged.algorithm_value=alg;merged.path=path;merged.record_count=count;merged.fanout=fanout;merged.bloom=std::move(bloom);kept.push_back(std::move(merged));std::sort(kept.begin(),kept.end(),[](const run& a,const run& b){return a.sequence>b.sequence;});auto old=std::move(state.runs);state.runs=std::move(kept);auto saved=co_await write_manifest();if(!saved)co_return std::unexpected(saved.error());for(const auto& source:old)if(std::find(input.begin(),input.end(),&source)!=input.end()){std::error_code ec;std::filesystem::remove(source.path,ec);}co_return result<void>{};
}

runtime::task<void> chunk_index::write_manifest(){std::vector<std::byte> bytes;bytes.insert(bytes.end(),manifest_magic.begin(),manifest_magic.end());std::uint32_t count=0;for(const auto& [ignored,state]:states_)count+=static_cast<std::uint32_t>(state.runs.size());put32(bytes,count);put32(bytes,highest_pack_id_);for(const auto& [alg,state]:states_)for(const auto& source:state.runs){bytes.push_back(std::byte{static_cast<unsigned char>(alg)});bytes.insert(bytes.end(),7,std::byte{0});put64(bytes,source.sequence);put64(bytes,source.record_count);}put64(bytes,check(bytes));const auto temporary=root_/"MANIFEST.tmp";auto saved=co_await write_file(temporary,std::move(bytes));if(!saved)co_return std::unexpected(saved.error());try{std::filesystem::rename(temporary,root_/"MANIFEST");}catch(const std::exception& e){co_return std::unexpected(failure(errc::io,e.what(),root_/"MANIFEST"));}co_return result<void>{};}
runtime::task<void> chunk_index::rebuild(std::vector<index_entry> entries){try{std::filesystem::remove_all(root_);std::filesystem::create_directories(root_);states_.clear();next_sequence_=1;for(auto& entry:entries){auto saved=co_await insert(std::move(entry));if(!saved)co_return std::unexpected(saved.error());}co_return co_await flush();}catch(const std::exception& e){co_return std::unexpected(failure(errc::io,e.what(),root_));}}
runtime::task<void> chunk_index::close(){auto saved=co_await flush();if(!saved)co_return std::unexpected(saved.error());for(auto& [ignored,state]:states_)for(auto& source:state.runs)if(source.file){source.file->close();source.file.reset();}co_return result<void>{};}
}  // namespace liquid::store_detail
