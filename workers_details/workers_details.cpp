//В маленькой мастерской три рабочих осуществляют окончательную сборку некоторого устройства из
//полуфабриката, установленного в тисках, закрепляя на нем две одинаковые гайки и один винт.Два рабочих умеют
//обращаться только с гаечным ключом, а один — только с отверткой.Действия рабочих схематически описываются
//следующим образом : взять элемент крепежа и, при наличии возможности, установить его на устройство; если все
//три элемента крепежа установлены, то вынуть из тисков готовое устройство и закрепить в них очередной
//полуфабрикат.Размеры устройства позволяют всем рабочим работать одновременно.Используя mutex и два семафора дейкстры, 
// постройте корректную модель сборки устройств с помощью трех потоков : по одному для каждого из рабочих.

#include <memory>
#include <print>
#include <pthread.h>
#include <string>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>

class Detail {
	friend class Work;
	pthread_mutex_t lock;
	std::size_t details_done=0;
	std::size_t workers_started=0;
	std::size_t workers_finished=0;
	std::size_t details_needed;
	std::string s_tool;
	std::string s_detail;

public:
	Detail(std::size_t d_needed, const std::string& s_t, const std::string& s_d) :
		details_needed(d_needed), s_tool(s_t), s_detail(s_d) {
		pthread_mutex_init(&lock, NULL);
	}
};

class Work {
	static pthread_mutex_t general_lock;
	static pthread_cond_t cond;
	static pthread_cond_t cond_operations;
	static std::size_t current_detail_idx;
	static std::vector<std::shared_ptr<Detail>> details;
	static std::size_t num_details;
	static std::size_t total_operations_per_detail;
	static std::size_t started_operations_per_detail;
	static std::size_t finished_operations_per_detail;


	std::size_t worker_id;
public:
	static void Init(const std::vector<std::shared_ptr<Detail>>& d, std::size_t n_d) {
		details=d;
		num_details=n_d;
		pthread_mutex_init(&general_lock, NULL);
		pthread_cond_init(&cond, NULL);
		pthread_cond_init(&cond_operations, NULL);

		current_detail_idx=0;
		total_operations_per_detail=0;
		started_operations_per_detail=0;
		finished_operations_per_detail=0;
		for (auto& d:details) {
			total_operations_per_detail+=d->details_needed;
		}
	}

	Work() {
		worker_id=gettid();
	}

	bool try_work(std::shared_ptr<Detail> d) {
		pthread_mutex_lock(&d->lock);
		if (d->details_done>=d->details_needed) {
			pthread_mutex_unlock(&d->lock);
			return false;
		}
		if (d->workers_started<d->details_needed) {
			started_operations_per_detail++;
			pthread_mutex_unlock(&general_lock);
			d->workers_started++;
			std::println("Worker {} took [{}] for {} ({}/{})", worker_id, d->s_tool, d->s_detail, d->workers_started, d->details_needed);
			pthread_mutex_unlock(&d->lock);
			std::println("Worker {} [{}] STARTED DOING work", worker_id, d->s_tool);
			usleep(200000); // 200ms
			std::println("Worker {} [{}] FINISHED DOING work", worker_id, d->s_tool);
			pthread_mutex_lock(&d->lock);
			d->workers_finished++;
			if (d->workers_finished>=d->details_needed) {
				d->details_done++;
				d->workers_started=0;
				d->workers_finished=0;
				std::println(">>> DETAIL FINISHED: {}", d->s_detail);
			}
			pthread_mutex_unlock(&d->lock);
			return true;
		}
		pthread_mutex_unlock(&d->lock);
		return false;
	}

	void run_work_loop() {
		while (true) {
			pthread_mutex_lock(&general_lock);
			// end work
			if (current_detail_idx>=num_details) {
				pthread_mutex_unlock(&general_lock);
				break;
			}
			for (auto& d:details) {
				// if return true, unlocked (lock). else locked (lock)
				if (try_work(d)) {
					pthread_mutex_lock(&general_lock);
					finished_operations_per_detail++;
					pthread_cond_broadcast(&cond_operations);
					break;
				}
			}
			if (started_operations_per_detail>=total_operations_per_detail) {
				// wait for last worker to continue
				if (finished_operations_per_detail<total_operations_per_detail) {
					pthread_cond_wait(&cond, &general_lock);
				}
				// last worker finished all operations
				else {
					std::println("\n!!! DETAIL #{} FULLY ASSEMBLED !!!\n", current_detail_idx);
					current_detail_idx++;
					started_operations_per_detail=0;
					finished_operations_per_detail=0;
					for (auto& d:details) {
						d->details_done=0;
					}
					pthread_cond_broadcast(&cond);
					pthread_cond_broadcast(&cond_operations);
					pthread_mutex_unlock(&general_lock);
					continue;
				}
			}
			// wait for any realesed worker to continue
			else {
				pthread_cond_wait(&cond_operations, &general_lock);
			}
			pthread_mutex_unlock(&general_lock);
		}
		std::println("Worker {} finished.", worker_id);
	}
};

pthread_mutex_t Work::general_lock;
std::size_t Work::current_detail_idx;
std::vector<std::shared_ptr<Detail>> Work::details;
std::size_t Work::num_details;
pthread_cond_t Work::cond;
pthread_cond_t Work::cond_operations;
std::size_t Work::total_operations_per_detail;
std::size_t Work::started_operations_per_detail;
std::size_t Work::finished_operations_per_detail;

void* run(void*) {
	Work w;
	w.run_work_loop();
	return nullptr;
}

int main() {
	auto bolts=std::make_shared<Detail>(2, "WRENCH", "BOLTS");
	auto screw=std::make_shared<Detail>(1, "SCREWDRIVER", "SCREW");
	std::vector<std::shared_ptr<Detail>> details_list={bolts, screw};
	const int num_details=3;
	const int num_workers=3;
	Work::Init(details_list, num_details);
	std::vector<pthread_t> threads(num_workers);
	for (int i=0; i<num_workers; ++i) {
		pthread_create(&threads[i], NULL, run, NULL);
	}
	for (int i=0; i<num_workers; ++i) {
		pthread_join(threads[i], NULL);
	}
	return 0;
}