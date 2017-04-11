#include <fstream>
#include <iostream>
#include <boost/iostreams/filtering_streambuf.hpp>
#include <boost/iostreams/copy.hpp>
#include <boost/iostreams/filter/gzip.hpp>
#include <unordered_map>
#include <vector>
#include <chrono>
#include <thread>
#include <future>

using namespace std;

struct pairhash {
public:
    template <typename T, typename U>
    std::size_t operator()(const std::pair<T, U> &x) const
    {
        return std::hash<T>()(x.first) ^ std::hash<U>()(x.second);
    }
};

std::unordered_map<std::string, int> uuid_map;
std::unordered_map<std::pair<int, int>, float, pairhash> user_topic_ref;


int get_uid(std::string &uuid) {
    int uid;
    auto pair = uuid_map.find(uuid);
    if (pair != uuid_map.end()) {
        uid = pair->second;
    } else {
        uid = uuid_map.size();
        uuid_map.insert(make_pair(uuid, uid));
    }
    return uid;
}


std::unordered_map<int, std::vector<std::pair<int, float>>> gen_doc_topic_map()
{
    std::unordered_map<int, std::vector<std::pair<int, float>>> doc_topic;
    string filename = "/home/yhk00323/input/documents_topics.csv.gz";
    //string filename = "/Users/heekyungyoon/Projects/feature_engineering_outbrain/data/documents_topics.csv.gz";
    std::string document_id;
    std::string topic_id;
    std::string confidence_level;
    std::string others;

    std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
    std::cout << "Start processing " << filename << std::endl;

    ifstream topic_file(filename, ios_base::in | ios_base::binary);
    boost::iostreams::filtering_streambuf<boost::iostreams::input> topic_inbuf;
    topic_inbuf.push(boost::iostreams::gzip_decompressor());
    topic_inbuf.push(topic_file);
    std::istream topic_instream(&topic_inbuf);

    std::getline(topic_instream, others);
    std::cout << "  Headers: " << others << std::endl;

    // transform to unordered map
    int i = 0;
    while(std::getline(topic_instream, document_id, ',')) {
        std::getline(topic_instream, topic_id, ',');
        std::getline(topic_instream, confidence_level);

        auto item = doc_topic.find(stoi(document_id));
        if (item != doc_topic.end()) {
            item->second.push_back(make_pair(stoi(topic_id), stof(confidence_level)));
        } else {
            std::vector<std::pair<int, float>> v;
            v.push_back(make_pair(stoi(topic_id), stof(confidence_level)));
            doc_topic.insert({stoi(document_id), v});
        }
        ++i;
    }

    topic_file.close();

    std::cout << "\ni = " << i <<"\nTime taken (sec): "
              << std::chrono::duration_cast<std::chrono::seconds>
                      (std::chrono::steady_clock::now() - begin).count()
              << "\n"
              << std::endl;

    return doc_topic;
};


void gen_user_topic_map(
        int tid,
        std::unordered_map<std::pair<int, int>, float, pairhash> *user_topic_map,
        std::string filename,
        int start_row,
        int end_row,
        std::unordered_map<int, std::vector<std::pair<int, float>>> *doc_topic_map)
{
    std::string uuid;
    std::string document_id;
    std::string others;

    // I. calculate user-topic interaction based on page_views
    std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
    std::cout << tid << "Start processing " << filename << std::endl;

    ifstream file(filename, ios_base::in | ios_base::binary);
    boost::iostreams::filtering_streambuf<boost::iostreams::input> inbuf;
    inbuf.push(boost::iostreams::gzip_decompressor());
    inbuf.push(file);
    std::istream instream(&inbuf);

    // transform to unordered map
    std::getline(instream, others);
    std::cout << "  Headers: " << others << std::endl;

    // skip rows until start row
    int i = 0; //all rows
    while(i < start_row - 1) {
        std::getline(instream, others);
        ++i;
    }
    // start processing
    int row_count = 0; //processed rows
    while(std::getline(instream, uuid, ',') && i < end_row) {
        std::getline(instream, document_id, ',');
        std::getline(instream, others);

        auto user = uuid_map.find(uuid);  // convert string uuid to int uid to save memory
        // if the document has topics associated with it
        auto document = (*doc_topic_map).find(stoi(document_id));
        if (user != uuid_map.end() && document != (*doc_topic_map).end()) {
            for (auto &t: document->second) {
                //if user topic exists in the reference
                auto user_topic = user_topic_ref.find(make_pair(user->second, t.first));
                if (user_topic != user_topic_ref.end()) {
                    auto user_topic2 = (*user_topic_map).find(make_pair(user->second, t.first));
                    if (user_topic2 != (*user_topic_map).end()) {
                        // if user topic exists in the map
                        user_topic2->second += t.second;
                    } else {
                        // if not
                        (*user_topic_map).insert({make_pair(user->second, t.first), t.second});
                    }

                }
            }
        }
        if (i % 10000000 == 0) {
            std::cout << "[" <<start_row << "]" << i/10000000 << "0M...";
            std::cout.flush();
        }
        ++row_count;
        ++i;
    }
    //Cleanup
    file.close();

    std::cout << "\nrow_count = " << row_count <<" (" << start_row << " - " << end_row << ")"
              << "\nTime taken (sec): "
              << std::chrono::duration_cast<std::chrono::seconds>
                      (std::chrono::steady_clock::now() - begin).count()
              << "\n"
              << std::endl;
}


std::vector<unordered_map<std::pair<int, int>, float, pairhash>> gen_user_topic_map_set(
        std::unordered_map<int, std::vector<std::pair<int, float>>> *doc_topic_map)
{
    std::vector<unordered_map<std::pair<int, int>, float, pairhash>> user_topic_map_set;
    string filename = "/home/yhk00323/input/page_views.csv.gz";
    //string filename = "/home/yhk00323/input/page_views_sample.csv.gz";

    unsigned int num_thread = 5;
    int num_row = 2034275448/num_thread + 1; //406,855,090

    //init user_topic_map
    for (int i = 0; i < num_thread; ++i) {
        unordered_map<std::pair<int, int>, float, pairhash> user_topic_map;
        user_topic_map_set.push_back(user_topic_map);
    }

    //start thread
    std::vector<std::thread> thread_list;
    for (int i = 0; i < num_thread; ++i) {
        thread_list.push_back(std::thread(gen_user_topic_map,
                                i,
                                &user_topic_map_set[i],
                                filename,
                                (i * num_row + 1),
                                ((1 + i) * num_row),
                                &(*doc_topic_map)));
    }

    //finish thread
    for (auto &t: thread_list) {
        t.join();
    }
    return user_topic_map_set;
}


std::unordered_map<int, std::pair<int, int>> gen_display_map(
        std::unordered_map<int, std::vector<std::pair<int, float>>> *doc_topic_map)
{
    // read events to get uuid and document id from clicks_train
    std::unordered_map<int, std::pair<int, int>> display_map;
    string filename = "/home/yhk00323/input/events.csv.gz";
    std::string display_id;
    std::string uuid;
    std::string document_id;
    std::string others;

    std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
    std::cout << "Start processing " << filename << std::endl;

    ifstream file(filename, ios_base::in | ios_base::binary);
    boost::iostreams::filtering_streambuf<boost::iostreams::input> inbuf;
    inbuf.push(boost::iostreams::gzip_decompressor());
    inbuf.push(file);
    std::istream instream(&inbuf);

    std::getline(instream, others);
    std::cout << "  Headers: " << others << std::endl;

    int i = 0; //rows
    while(std::getline(instream, display_id, ',')) {
        std::getline(instream, uuid, ',');
        std::getline(instream, document_id, ',');
        std::getline(instream, others);
        int uid = get_uid(uuid);

        //insert all display ids to display map
        display_map.insert({stoi(display_id), std::make_pair(uid, stoi(document_id))});
        //save all user-topic pair associated with display ids in events.csv (afterwards, won't process what's not in it)
        //if the document has topics associated with it
        auto document = (*doc_topic_map).find(stoi(document_id));
        if (document != (*doc_topic_map).end()) {
            for (auto &t: document->second) {
                //if user topic doesn't exist
                auto user_topic = user_topic_ref.find(make_pair(uid, t.first));
                if (user_topic == user_topic_ref.end()) {
                    user_topic_ref.insert({make_pair(uid, t.first), 0});
                }
            }
        }

        if (i % 1000000 == 0)
            std::cout << i/1000000 << "M...";
            std::cout.flush();
        ++i;
    }

    file.close();

    std::cout << "\ni = " << i <<"\nTime taken (sec): "
              << std::chrono::duration_cast<std::chrono::seconds>
                      (std::chrono::steady_clock::now() - begin).count()
              << "\n"
              << std::endl;
    return display_map;
}


int calc_user_doc_interaction_topic(
        std::unordered_map<int, std::vector<std::pair<int, float>>> *doc_topic_map,
        std::vector<unordered_map<std::pair<int, int>, float, pairhash>> *user_topic_map_set,
        std::unordered_map<int, std::pair<int, int>> *display_map
)
{
    // read clicks_train
    string filename = "/home/yhk00323/input/clicks_test.csv.gz";
    std::string display_id;
    std::string others;

    std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
    std::cout << "Start processing " << filename << std::endl;

    ifstream test_file(filename, ios_base::in | ios_base::binary);
    boost::iostreams::filtering_streambuf<boost::iostreams::input> test_inbuf;
    test_inbuf.push(boost::iostreams::gzip_decompressor());
    test_inbuf.push(test_file);
    std::istream test_instream(&test_inbuf);

    // transform to unordered map
    std::getline(test_instream, others);
    std::cout << "  Headers: " << others << std::endl;

    // write interaction weights
    std::ofstream outfile("clicks_test_doc_topic_weight.csv.gz", std::ios_base::out | std::ios_base::binary);
    boost::iostreams::filtering_streambuf<boost::iostreams::output> out;
    out.push(boost::iostreams::gzip_compressor());
    out.push(outfile);
    std::ostream outstream(&out);

    outstream << "weight\n";

    // for row
    // read clicks_train row
    // save interaction to separate file
    int i = 0;
    while(std::getline(test_instream, display_id, ',')) {
        std::getline(test_instream, others);
        //calculate weight
        float weight = 0.0;
        // if uuid and document id related to the display_id exists
        auto display = (*display_map).find(stoi(display_id));
        if (display != (*display_map).end()) {
            // if topic id related to the document id exists
            auto document = (*doc_topic_map).find(display->second.second);
            if (document != (*doc_topic_map).end()) {
                for (auto &dt: document->second) {
                    // if topic id related to the user id exists
                    for (auto &ut_map: (*user_topic_map_set)) {
                        auto user_topic = ut_map.find(make_pair(display->second.first, dt.first));
                        if (user_topic != ut_map.end()) {
                            weight += user_topic->second;
                        }
                    }
                }
            }
        }
        outstream << weight <<"\n";
        ++i;
    }

    test_file.close();

    std::cout << "\ni = " << i <<"\nTime taken (sec): "
              << std::chrono::duration_cast<std::chrono::seconds>
                      (std::chrono::steady_clock::now() - begin).count()
              << "\n"
              << std::endl;
    return 0;
}




int main() {
    // I. Read file
    // <document_id, <topic_id, confidence_level>>
    std::unordered_map<int, std::vector<std::pair<int, float>>> doc_topic_map = gen_doc_topic_map();
    // <display_id, <uuid, document_id>>
    std::unordered_map<int, std::pair<int, int>> display_map = gen_display_map(&doc_topic_map);
    // <<uuid, topic_id>, sum_confidence_level>
    std::vector<unordered_map<std::pair<int, int>, float, pairhash>> user_topic_map_set = gen_user_topic_map_set(
            &doc_topic_map);

    // II. calculate user-document interaction in terms of topic
    calc_user_doc_interaction_topic(&doc_topic_map, &user_topic_map_set, &display_map);

    return 0;
}
