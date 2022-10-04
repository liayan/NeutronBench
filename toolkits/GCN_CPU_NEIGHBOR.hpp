#include "core/neutronstar.hpp"
#include "core/ntsPeerRPC.hpp"
#include "utils/torch_func.hpp"

class GCN_CPU_NEIGHBOR_impl {
public:
  int iterations;
  ValueType learn_rate;
  ValueType weight_decay;
  ValueType drop_rate;
  ValueType alpha;
  ValueType beta1;
  ValueType beta2;
  ValueType epsilon;
  ValueType decay_rate;
  ValueType decay_epoch;
  int layers;
  // graph
  VertexSubset *active;
  // graph with no edge data
  Graph<Empty> *graph;
  //std::vector<CSC_segment_pinned *> subgraphs;
  // NN
  GNNDatum *gnndatum;
  NtsVar L_GT_C;
  NtsVar L_GT_G;
  NtsVar MASK;
  //GraphOperation *gt;
  PartitionedGraph *partitioned_graph;
  // Variables
  std::vector<Parameter *> P;
  std::vector<NtsVar> X;
  nts::ctx::NtsContext* ctx;
  FullyRepGraph* fully_rep_graph;
  double train_sample_time = 0;
  double train_compute_time = 0;
  double mpi_comm_time = 0;
  double rpc_comm_time = 0;
  double rpc_wait_time = 0;
  float loss_epoch = 0;

  
  NtsVar F;
  NtsVar loss;
  NtsVar tt;
  float acc;
  int batch;
  long correct;
  long train_nodes;
  int max_batch_num;
  int min_batch_num;
  torch::nn::Dropout drpmodel;
  // double sample_cost = 0;
  std::vector<torch::nn::BatchNorm1d> bn1d;

  ntsPeerRPC<ValueType, VertexId> rpc;

  GCN_CPU_NEIGHBOR_impl(Graph<Empty> *graph_, int iterations_,
               bool process_local = false, bool process_overlap = false) {
    graph = graph_;
    iterations = iterations_;

    active = graph->alloc_vertex_subset();
    active->fill();

    graph->init_gnnctx(graph->config->layer_string);
    graph->init_gnnctx_fanout(graph->config->fanout_string);
    // rtminfo initialize
    graph->init_rtminfo();
    graph->rtminfo->process_local = graph->config->process_local;
    graph->rtminfo->reduce_comm = graph->config->process_local;
    graph->rtminfo->copy_data = false;
    graph->rtminfo->process_overlap = graph->config->overlap;
    graph->rtminfo->with_weight = true;
    graph->rtminfo->with_cuda = false;
    graph->rtminfo->lock_free = graph->config->lock_free;
  }
  
  void init_active() {
    active = graph->alloc_vertex_subset();
    active->fill();
  }

  void init_graph() {
    fully_rep_graph=new FullyRepGraph(graph);
    fully_rep_graph->GenerateAll();
    fully_rep_graph->SyncAndLog("read_finish");
       
    //cp = new nts::autodiff::ComputionPath(gt, subgraphs);
    ctx=new nts::ctx::NtsContext();
  }

  void get_batch_num() {
    VertexId max_vertex = 0;
    VertexId min_vertex = std::numeric_limits<VertexId>::max();
    for(int i = 0; i < graph->partitions; i++){
        max_vertex = std::max(graph->partition_offset[i+1] - graph->partition_offset[i], max_vertex);
        min_vertex = std::min(graph->partition_offset[i+1] - graph->partition_offset[i], min_vertex);
    }
    max_batch_num = max_vertex / graph->config->batch_size;
    min_batch_num = min_vertex / graph->config->batch_size;
    if(max_vertex % graph->config->batch_size != 0) {
        max_batch_num++;
    }
    if(min_vertex % graph->config->batch_size != 0) {
        min_batch_num++;
    }
  }

  void init_nn() {
    learn_rate = graph->config->learn_rate;
    weight_decay = graph->config->weight_decay;
    drop_rate = graph->config->drop_rate;
    alpha = graph->config->learn_rate;
    decay_rate = graph->config->decay_rate;
    decay_epoch = graph->config->decay_epoch;
    layers = graph->gnnctx->layer_size.size() - 1;
    beta1 = 0.9;
    beta2 = 0.999;
    epsilon = 1e-9;
    gnndatum = new GNNDatum(graph->gnnctx, graph);
    // gnndatum->random_generate();
    // std::cout << "start nn" << std::endl;
    if (0 == graph->config->feature_file.compare("random")) {
      gnndatum->random_generate();
    } else {
      gnndatum->readFeature_Label_Mask(graph->config->feature_file,
                                       graph->config->label_file,
                                       graph->config->mask_file);
    }
    // std::cout << "read done" << std::endl;

    // creating tensor to save Label and Mask
    if (graph->config->classes > 1) {
      gnndatum->registLabel(L_GT_C, gnndatum->local_label, gnndatum->gnnctx->l_v_num, graph->config->classes);
      // std::cout << "10 " << L_GT_C[10] << std::endl;
      // for (int i = 0; i < 121; ++i) {
      //   std::cout << gnndatum->local_label[100 * 121 + i] << " ";
      // }std::cout << std::endl;
      // std::cout << "100 " << L_GT_C[100] << std::endl;
      // assert(false);
    } else {
      gnndatum->registLabel(L_GT_C);
    }
    // std::cout << L_GT_C << std::endl;
    gnndatum->registMask(MASK);

    // initializeing parameter. Creating tensor with shape [layer_size[i],
    // layer_size[i + 1]]
    // std::cout << graph->gnnctx->layer_size.size() << std::endl;
    // assert (false);

    for (int i = 0; i < layers; i++) {
      P.push_back(new Parameter(graph->gnnctx->layer_size[i], graph->gnnctx->layer_size[i + 1], alpha, beta1, beta2, epsilon, weight_decay));
      if(graph->config->batch_norm && i < layers - 1) {
        // std::cout << "use batch norm" << std::endl;
        bn1d.push_back(torch::nn::BatchNorm1d(graph->gnnctx->layer_size[i])); 
      }
  
    }

    // synchronize parameter with other processes
    // because we need to guarantee all of workers are using the same model
    for (int i = 0; i < P.size(); i++) {
      P[i]->init_parameter();
      P[i]->set_decay(decay_rate, decay_epoch);
    }

    drpmodel = torch::nn::Dropout(torch::nn::DropoutOptions().p(drop_rate).inplace(true));

    F = graph->Nts->NewLeafTensor(
        gnndatum->local_feature,
        {graph->gnnctx->l_v_num, graph->gnnctx->layer_size[0]},
        torch::DeviceType::CPU);

    // X[i] is vertex representation at layer i
    for (int i = 0; i < layers + 1; i++) {
      NtsVar d;
      X.push_back(d);
    }
    
    X[0] = F.set_requires_grad(true);

    rpc.set_comm_num(graph->partitions - 1);
    rpc.register_function("get_feature", [&](std::vector<VertexId> vertexs){
        int start = graph->partition_offset[graph->partition_id];
        int feature_size = F.size(1);
        ValueType* ntsVarBuffer = graph->Nts->getWritableBuffer(F, torch::DeviceType::CPU);
        std::vector<std::vector<ValueType>> result_vector;
        result_vector.resize(vertexs.size());

        #pragma omp parallel for
        for(int i = 0; i < vertexs.size(); i++) {
            result_vector[i].resize(feature_size);
            memcpy(result_vector[i].data(), ntsVarBuffer + (vertexs[i] - start) * feature_size,
                   feature_size * sizeof(ValueType));
        }
        return result_vector;
    });
  }

  void Update() {
    for (int i = 0; i < P.size(); i++) {
    // accumulate the gradient using all_reduce
    if (ctx->is_train() && graph->rtminfo->epoch >= 3) mpi_comm_time -= get_time();
      if (graph->gnnctx->l_v_num == 0) {
        P[i]->all_reduce_to_gradient(torch::zeros_like(P[i]->W));
      } else {
        P[i]->all_reduce_to_gradient(P[i]->W.grad().cpu());
      }
    if (ctx->is_train() && graph->rtminfo->epoch >= 3) mpi_comm_time += get_time();
    // update parameters with Adam optimizer
      P[i]->learnC2C_with_decay_Adam();
      // P[i]->learnC2C_with_Adam();
      P[i]->next();
    }
  }

  void UpdateZero() {
    for(int l=0;l<layers;l++){
//          std::printf("process %d epoch %d last before\n", graph->partition_id, curr_epoch);
        P[l]->all_reduce_to_gradient(torch::zeros({P[l]->row, P[l]->col}, torch::kFloat));
//          std::printf("process %d epoch %d last after\n", graph->partition_id, curr_epoch);
        // P[l]->learnC2C_with_decay_Adam();
        // P[l]->next();
    }
  }
  
  float Forward(Sampler* sampler, int type=0) {
    graph->rtminfo->forward = true;
    if (graph->config->batch_type != SEQUENCE) { // shuffle
      // printf("shuffle sample nodes\n");
      shuffle_vec(sampler->sample_nids);
    }  
    // node sampling
    // LOG_DEBUG("type %d, start sample", type);
    double sample_cost = -get_time();
    while(sampler->sample_not_finished()) {
      // sampler->reservoir_sample(layers,
      //                           graph->config->batch_size,
      //                           graph->gnnctx->fanout);
      // printf("batch_type %d\n", graph->config->batch_type);
      // double tmp_start = -get_time();
      sampler->reservoir_sample(layers,
                                graph->config->batch_size,
                                graph->gnnctx->fanout, graph->config->batch_type, ctx->is_train());
      // printf("# sample_one cost %.3f\n", tmp_start + get_time());
      // assert(tmp_cnt++ < 3);
    }
    sample_cost += get_time();
    // LOG_DEBUG("sample cost %.3f", sample_cost);

    if (type ==  0 && graph->rtminfo->epoch >= 3) train_sample_time += sample_cost;
    int batch_num = sampler->size();
    LOG_DEBUG("sample_nodes %d batch_num %d sample_cost %.3f", sampler->sample_nids.size(), batch_num, sample_cost);
    // printf("## sample_nodes %d batch_num %d sample_cost %.3f\n", sampler->sample_nids.size(), batch_num, sample_cost);
  
    if (type == 0 && graph->rtminfo->epoch >= 3) mpi_comm_time -= get_time();
    MPI_Allreduce(&batch_num, &max_batch_num, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
    MPI_Allreduce(&batch_num, &min_batch_num, 1, MPI_INT, MPI_MIN, MPI_COMM_WORLD);
    if (type == 0 && graph->rtminfo->epoch >= 3) mpi_comm_time += get_time();
    // printf("debug comm batch_num is done\n");
    // printf("batch_num %d min %d max %d\n", batch_num, min_batch_num, max_batch_num);
    // sampler->isValidSampleGraph();
    // std::cout << type << " neighbor sample done" << std::endl;

    SampledSubgraph *sg; 
    correct = 0;
    train_nodes = 0;
    batch=0;
    // if(min_batch_num == 0) {
    //     rpc.keep_running();
    // }
    // std::cout << min_batch_num << " " << max_batch_num << std::endl;
    double forward_cost = -get_time();
    double tmp_time = 0;
    float f1_epoch = 0;
    loss_epoch = 0;

    double nn_cost = 0;
    double update_degree_cost = 0;
    while(sampler->has_rest()){
        sg=sampler->get_one();

        std::vector<NtsVar> X;
        // LOG_DEBUG("forward graph address %p", graph);
        NtsVar d;
        X.resize(graph->gnnctx->layer_size.size(),d);
      
        // X[0]=nts::op::get_feature(sg->sampled_sgs[0]->src(),F,graph);
        // rpc.keep_running();
        tmp_time = -get_time();
        X[0] = nts::op::get_feature_from_global(rpc, sg->sampled_sgs[0]->src(), F, graph);
        tmp_time += get_time();
        if (type == 0 && graph->rtminfo->epoch >= 3) rpc_comm_time += tmp_time;  
        // std::cout << batch_num << " "<< batch <<  " get feature done" << std::endl;
        // rpc.stop_running();
      //  std::cout << "get feature done" << std::endl;
        NtsVar target_lab=nts::op::get_label(sg->sampled_sgs.back()->dst(), L_GT_C, graph);
      //  std::cout << "get label done" << std::endl;

        // if (type == 0) {
        //   std::cout << "train ";
        // } else {
        //   std::cout << "val ";
        // }

        // std::cout << "label (" << sg->sampled_sgs.back()->dst().size() << "): "; 
        // for (auto v : sg->sampled_sgs.back()->dst()) {
        //   std::cout << v << " ";
        // } std::cout << std::endl;


      //  graph->rtminfo->forward = true;
        for(int l = 0; l < layers; l++) {//forward
          //  int hop=(graph->gnnctx->layer_size.size()-2)-l;
        //  std::cout << "start process layer " << l << std::endl;
            // if (l > 0) {
            //   cout << "dopout" << std::endl;
            //   X[l] = torch::dropout(X[l], drop_rate, ctx->is_train());
            // }
            update_degree_cost -= get_time();
            sg->update_degrees(graph, l);
            update_degree_cost += get_time();

            NtsVar Y_i=ctx->runGraphOp<nts::op::MiniBatchFuseOp>(sg, graph, l, X[l]);
            // printf("run graphop done\n");
            X[l + 1]=ctx->runVertexForward([&](NtsVar n_i) {
              // if (l==(graph->gnnctx->layer_size.size()-2)) {
              if (l == layers - 1) { // last layer
                // std::cout << "last layer" << std::endl;
                return P[l]->forward(n_i);
                // return torch::mean(P[l]->forward(n_i));
              }else{
                // std::cout << "inter layer" << std::endl;
                if (graph->config->batch_norm) {
                  // std::cout << "use bn1d" << std::endl;
                  n_i = this->bn1d[l](n_i); // for arxiv dataset
                }
                // return torch::dropout(P[l]->forward(n_i), drop_rate, ctx->is_train());
                return torch::dropout(torch::relu(P[l]->forward(n_i)), drop_rate, ctx->is_train());
                // return torch::relu(P[l]->forward(n_i));
              }
            },
            Y_i);
            // printf("run vertex forward done\n");
        } 

        // std::cout << "start loss" << std::endl;
        // std::cout << X[layers] << std::endl;
        auto loss_ = Loss(X[layers], target_lab, graph->config->classes == 1);
        loss_epoch += loss_.item<float>();
        // std::cout << "loss: " << loss_.item<float>() << " loss_epoch " << loss_epoch << std::endl;
        // std::cout << loss_.item<float>() << std::endl;
        // std::cout << "loss after " << loss_.data_ptr() << std::endl;

        if (ctx->training == true) {
          ctx->appendNNOp(X[layers], loss_);
        }

        // std::cout << "start backward" << std::endl;
        if (ctx->training) {
          ctx->self_backward(false);
          // std::cout << "backward done" << std::endl;
          Update();
          // std::cout << "update done" << std::endl;
          for (int i = 0; i < P.size(); i++) {
            P[i]->zero_grad();
          }
          // std::cout << "zero_grad done" << std::endl;
        }
        // std::cout << "backward done" << std::endl;
        // std::cout << "loss done" << std::endl;
        correct += get_correct(X[layers], target_lab, graph->config->classes == 1);
        // std::cout << "correct done" << std::endl;
        // std::cout << "layuer " << layers << std::endl;
        // assert (false);
        float f1 = f1_score(X[layers], target_lab, graph->config->classes == 1);
        f1_epoch += f1;

        // std::cout << X[layers] << std::endl << target_lab << std::endl;
        // std::cout << correct << std::endl;
        // std::cout << "f1 " << f1 << " " << f1_epoch << std::endl;
        // assert(false);
        train_nodes += target_lab.size(0);

        // sg->sampled_sgs.back()->dst()
        // graph->rtminfo->forward = false;
        // if (ctx->training) {
        //   ctx->self_backward(false);
        //   // std::cout << "backward done" << std::endl;
        //   Update();
        //   // std::cout << "update done" << std::endl;
        //   for (int i = 0; i < P.size(); i++) {
        //     P[i]->zero_grad();
        //   }
        //   // std::cout << "zero_grad done" << std::endl;
        // }
        
        batch++;
        // if(batch == min_batch_num) {
        //   rpc.keep_running();
        // }
    }
    loss_epoch /= batch_num;
    // LOG_DEBUG("avg loss %.4f", loss_epoch);
    // std::cout << "train " << ctx->training << " batch " << batch << " max_batch_num " << max_batch_num << std::endl;
    // max_batch_num = 50;
    if (type == 0 && graph->rtminfo->epoch >= 3) rpc_wait_time -= get_time();
    while(ctx->training && batch !=max_batch_num){
      UpdateZero();
      // std::cout << "sync update zero " << batch << std::endl;
      batch++;
    }
    rpc.stop_running();
    if (type == 0 && graph->rtminfo->epoch >= 3) rpc_wait_time += get_time();

    sampler->clear_queue();
    sampler->restart();
    // acc = 1.0 * correct / sampler->work_range[1];
    // std::cout << "before " << correct << " " << train_nodes << std::endl;
    if (type == 0 && graph->rtminfo->epoch >= 3)
      mpi_comm_time -= get_time();
    MPI_Allreduce(MPI_IN_PLACE, &correct, 1, MPI_LONG, MPI_SUM, MPI_COMM_WORLD);
    MPI_Allreduce(MPI_IN_PLACE, &train_nodes, 1, MPI_LONG, MPI_SUM, MPI_COMM_WORLD);
    if (type == 0 && graph->rtminfo->epoch >= 3)
     mpi_comm_time += get_time();
    acc = 1.0 * correct / train_nodes;
    // std::cout << "after " << correct << " " << train_nodes << std::endl;
    // printf("train_ndoes %d\n", train_nodes);
    forward_cost += get_time();
    LOG_DEBUG("forward cost %.3f", forward_cost);
    LOG_DEBUG("upadte_degree_cost %.5f", update_degree_cost);


    // if (graph->partition_id == 0)
    // printf("\thost %d batch_num %d sample_cost %.3f forward_cost %.3f\n", 
    //       graph->partition_id, batch_num, sample_cost, forward_cost);
    if (graph->config->classes > 1) {
      f1_epoch /= batch_num;
      acc = f1_epoch;
    }
    // std::cout << acc << " " << f1_epoch << std::endl;
    // assert(false);
    
    return acc;
    // return graph->config->classes > 1 ? f1 : acc;
  }

  void zero_grad() {
    // if (i_i != 0) {
      for (int i = 0; i < P.size(); i++) {
        P[i]->zero_grad();
      }
    // }
  }

  void shuffle_vec(std::vector<VertexId>& vec) {
    unsigned seed = std::chrono::system_clock::now ().time_since_epoch ().count();
    std::shuffle (vec.begin(), vec.end(), std::default_random_engine(seed));
  }

  float run() {
    double pre_time = -get_time();
    if (graph->partition_id == 0) {
      LOG_INFO("GNNmini::[Dist.GPU.GCNimpl] running [%d] Epoches\n",
               iterations);
    }
    // get train/val/test node index. (may be move this to GNNDatum)
    std::vector<VertexId> train_nids, val_nids, test_nids;
    BatchType batch_type = graph->config->batch_type;
    // std::cout << "l_v_num " << graph->gnnctx->l_v_num << std::endl;
    for (int i = 0; i < graph->gnnctx->l_v_num; ++i) {
    // for (int i = graph->partition_offset[graph->partition_id]; i < graph->partition_offset[graph->partition_id + 1]; ++i) {
      int type = gnndatum->local_mask[i];
      // std::cout << i << " " << type << " " << i + graph->partition_offset[graph->partition_id] << std::endl;
      if (type == 0) {
        train_nids.push_back(i + graph->partition_offset[graph->partition_id]);
      } else if (type == 1) {
        val_nids.push_back(i + graph->partition_offset[graph->partition_id]);
      } else if (type == 2) {
        test_nids.push_back(i + graph->partition_offset[graph->partition_id]);
      }
    }

    shuffle_vec(val_nids);
    shuffle_vec(test_nids);
    
    if (batch_type == SHUFFLE) shuffle_vec(train_nids);

    if (batch_type == RANDOM || batch_type == DELLOW || batch_type == DELHIGH) { // random
      if (batch_type == DELHIGH) {
         std::sort(train_nids.begin(), train_nids.end(), [&](const auto& x, const auto& y) {
          return graph->in_degree_for_backward[x] > graph->in_degree_for_backward[y];
        });
      } else if (batch_type == DELLOW){
        std::sort(train_nids.begin(), train_nids.end(), [&](const auto& x, const auto& y) {
          return graph->in_degree_for_backward[x] < graph->in_degree_for_backward[y];
        });
      } else { // random del nodes
        shuffle_vec(train_nids);
      }
      // std::cout << "LOW " << DELLOW << " " << batch_type << std::endl;
      // std::cout << graph->in_degree_for_backward[train_nids[0]] << " " << graph->in_degree_for_backward[train_nids[100]] << std::endl;
      // assert(false);
      int sz = train_nids.size();
      train_nids.erase(train_nids.begin() + static_cast<int>(sz * (1 - graph->config->del_frac)), train_nids.end());
      shuffle_vec(train_nids);
    }
    std::cout << "train/val/test: " << train_nids.size() << " " << val_nids.size() << " " << test_nids.size() << std::endl;
    Sampler* train_sampler = new Sampler(fully_rep_graph, train_nids);
    Sampler* eval_sampler = new Sampler(fully_rep_graph, val_nids);
    Sampler* test_sampler = new Sampler(fully_rep_graph, test_nids);
    pre_time += get_time();

    double train_time =  0;
    double val_time =  0;
    double test_time =  0;
    float best_val_acc = 0;
    double run_time = -get_time();
    for (int i_i = 0; i_i < iterations; i_i++) {
      graph->rtminfo->epoch = i_i;
      zero_grad();
      
      ctx->train();
      // std::cout << "start train" << std::endl;
      if (i_i >= graph->config->time_skip) train_time -= get_time();
      float train_acc = Forward(train_sampler, 0);
      // std::cout << "after train forward" << std::endl;
      float train_loss = loss_epoch;
      if (i_i >= graph->config->time_skip) train_time += get_time();
      // std::cout << "end train" << std::endl;
      
      ctx->eval();
      if (i_i >= graph->config->time_skip) val_time -= get_time();
      float val_acc = Forward(eval_sampler, 1);
      float val_loss = loss_epoch;
      best_val_acc = std::max(best_val_acc, val_acc);
      if (i_i >= graph->config->time_skip) val_time += get_time();

      // std::cout << "start test" << std::endl;
      // if (i_i >= graph->config->time_skip) test_time -= get_time();
      // float test_acc = Forward(test_sampler, 2);
      // if (i_i >= graph->config->time_skip) test_time += get_time();
      // std::cout << "end test" << std::endl;

      float test_acc = 0;
      if (graph->partition_id == 0) {
        // std::cout << loss_epoch << std::endl;
        printf("Epoch %03d loss %.3f train_acc %.3f val_acc %.3f test_acc %.3f\n\n", i_i, train_loss, train_acc, val_acc, test_acc);
        // LOG_INFO("Epoch %03d loss %.3f train_acc %.3f val_acc %.3f test_acc %.3f", i_i, train_loss, train_acc, val_acc, test_acc);
      }
    }
    printf("best val acc: %.3f\n", best_val_acc);

    double comm_time = mpi_comm_time + rpc_comm_time + rpc_wait_time;
    double compute_time = train_time - comm_time;
    printf("train TIME(%d) sample %.3f compute_time %.3f comm_time %.3f mpi_comm %.3f rpc_comm %.3f rpc_wait_time %.3f\n", 
          graph->partition_id, train_sample_time, compute_time, comm_time, mpi_comm_time, rpc_comm_time, rpc_wait_time);
    printf("train/val/test time: %.3f, %.3f, %.3f\n", train_time, val_time, test_time);

    
    
    rpc.exit();
    run_time += get_time();
    printf("run(): pre_time %.3fs runtime: %.3fs\n", pre_time, run_time);


    if (graph->partition_id == 0)
      printf("Avg epoch train_time %.3f val_time %.3f test_time %.3f\n", 
            train_time / (iterations - graph->config->time_skip), 
            val_time / (iterations - graph->config->time_skip), 
            test_time / (iterations - graph->config->time_skip));
    
    delete active;
    return best_val_acc;
  }
};