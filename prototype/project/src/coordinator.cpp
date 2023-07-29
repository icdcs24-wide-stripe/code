#include "coordinator.h"
#include "tinyxml2.h"
#include <random>
#include <unistd.h>
#include "lrc.h"
#include <sys/time.h>

template <typename T>
inline T ceil(T const &A, T const &B)
{
  return T((A + B - 1) / B);
};

template <typename T>
inline std::vector<size_t> argsort(const std::vector<T> &v)
{
  std::vector<size_t> idx(v.size());
  std::iota(idx.begin(), idx.end(), 0);
  std::sort(idx.begin(), idx.end(), [&v](size_t i1, size_t i2)
            { return v[i1] < v[i2]; });
  return idx;
};

inline int rand_num(int range)
{
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<int> dis(0, range - 1);
  int num = dis(gen);
  return num;
};

namespace ECProject
{
  grpc::Status CoordinatorImpl::setParameter(
      grpc::ServerContext *context,
      const coordinator_proto::Parameter *parameter,
      coordinator_proto::RepIfSetParaSuccess *setParameterReply)
  {
    ECSchema system_metadata(parameter->partial_decoding(),
                             (ECProject::EncodeType)parameter->encodetype(),
                             (ECProject::SingleStripePlacementType)parameter->s_stripe_placementtype(),
                             (ECProject::MultiStripesPlacementType)parameter->m_stripe_placementtype(),
                             parameter->k_datablock(),
                             parameter->l_localparityblock(),
                             parameter->g_m_globalparityblock(),
                             parameter->b_datapergroup(),
                             parameter->x_stripepermergegroup());
    m_encode_parameters = system_metadata;
    setParameterReply->set_ifsetparameter(true);
    m_cur_cluster_id = 0;
    m_cur_stripe_id = 0;
    m_object_commit_table.clear();
    m_object_updating_table.clear();
    m_stripe_deleting_table.clear();
    for (auto it = m_cluster_table.begin(); it != m_cluster_table.end(); it++)
    {
      Cluster &t_cluster = it->second;
      t_cluster.blocks.clear();
      t_cluster.stripes.clear();
    }
    for (auto it = m_node_table.begin(); it != m_node_table.end(); it++)
    {
      Node &t_node = it->second;
      t_node.stripes.clear();
    }
    m_stripe_table.clear();
    m_merge_groups.clear();
    m_free_clusters.clear();
    m_merge_degree = 0;
    m_agg_start_cid = 0;
    std::cout << "setParameter success" << std::endl;
    return grpc::Status::OK;
  }

  grpc::Status CoordinatorImpl::sayHelloToCoordinator(
      grpc::ServerContext *context,
      const coordinator_proto::RequestToCoordinator *helloRequestToCoordinator,
      coordinator_proto::ReplyFromCoordinator *helloReplyFromCoordinator)
  {
    std::string prefix("Hello ");
    helloReplyFromCoordinator->set_message(prefix + helloRequestToCoordinator->name());
    std::cout << prefix + helloRequestToCoordinator->name() << std::endl;
    return grpc::Status::OK;
  }

  grpc::Status CoordinatorImpl::uploadOriginKeyValue(
      grpc::ServerContext *context,
      const coordinator_proto::RequestProxyIPPort *keyValueSize,
      coordinator_proto::ReplyProxyIPPort *proxyIPPort)
  {

    std::string key = keyValueSize->key();
    m_mutex.lock();
    m_object_commit_table.erase(key);
    m_mutex.unlock();
    int valuesizebytes = keyValueSize->valuesizebytes();

    ObjectInfo new_object;

    int k = m_encode_parameters.k_datablock;
    int g_m = m_encode_parameters.g_m_globalparityblock;
    int l = m_encode_parameters.l_localparityblock;
    // int b = m_encode_parameters.b_datapergroup;
    new_object.object_size = valuesizebytes;
    int block_size = ceil(valuesizebytes, k);

    proxy_proto::ObjectAndPlacement object_placement;
    object_placement.set_key(key);
    object_placement.set_valuesizebyte(valuesizebytes);
    object_placement.set_k(k);
    object_placement.set_g_m(g_m);
    object_placement.set_l(l);
    object_placement.set_encode_type((int)m_encode_parameters.encodetype);
    object_placement.set_block_size(block_size);

    Stripe t_stripe;
    t_stripe.stripe_id = m_cur_stripe_id++;
    t_stripe.k = k;
    t_stripe.l = l;
    t_stripe.g_m = g_m;
    t_stripe.object_keys.push_back(key);
    t_stripe.object_sizes.push_back(valuesizebytes);
    m_stripe_table[t_stripe.stripe_id] = t_stripe;
    new_object.map2stripe = t_stripe.stripe_id;

    int s_cluster_id = generate_placement(t_stripe.stripe_id, block_size);

    Stripe &stripe = m_stripe_table[t_stripe.stripe_id];
    object_placement.set_stripe_id(stripe.stripe_id);
    for (int i = 0; i < int(stripe.blocks.size()); i++)
    {
      object_placement.add_datanodeip(m_node_table[stripe.blocks[i]->map2node].node_ip);
      object_placement.add_datanodeport(m_node_table[stripe.blocks[i]->map2node].node_port);
      object_placement.add_blockkeys(stripe.blocks[i]->block_key);
    }

    grpc::ClientContext cont;
    proxy_proto::SetReply set_reply;
    std::string selected_proxy_ip = m_cluster_table[s_cluster_id].proxy_ip;
    int selected_proxy_port = m_cluster_table[s_cluster_id].proxy_port;
    std::string chosen_proxy = selected_proxy_ip + ":" + std::to_string(selected_proxy_port);
    grpc::Status status = m_proxy_ptrs[chosen_proxy]->encodeAndSetObject(&cont, object_placement, &set_reply);
    proxyIPPort->set_proxyip(selected_proxy_ip);
    proxyIPPort->set_proxyport(selected_proxy_port + 1); // use another port to accept data
    if (status.ok())
    {
      m_mutex.lock();
      m_object_updating_table[key] = new_object;
      m_mutex.unlock();
    }
    else
    {
      std::cout << "[SET] Send object placement failed!" << std::endl;
    }

    return grpc::Status::OK;
  }

  grpc::Status CoordinatorImpl::getValue(
      grpc::ServerContext *context,
      const coordinator_proto::KeyAndClientIP *keyClient,
      coordinator_proto::RepIfGetSuccess *getReplyClient)
  {
    try
    {
      std::string key = keyClient->key();
      std::string client_ip = keyClient->clientip();
      int client_port = keyClient->clientport();
      ObjectInfo object_info;
      m_mutex.lock();
      object_info = m_object_commit_table.at(key);
      m_mutex.unlock();
      int k = m_encode_parameters.k_datablock;
      int g_m = m_encode_parameters.g_m_globalparityblock;
      int l = m_encode_parameters.l_localparityblock;
      // int b = m_encode_parameters.b_datapergroup;

      grpc::ClientContext decode_and_get;
      proxy_proto::ObjectAndPlacement object_placement;
      grpc::Status status;
      proxy_proto::GetReply get_reply;
      getReplyClient->set_valuesizebytes(object_info.object_size);
      object_placement.set_key(key);
      object_placement.set_valuesizebyte(object_info.object_size);
      object_placement.set_k(k);
      object_placement.set_l(l);
      object_placement.set_g_m(g_m);
      object_placement.set_stripe_id(object_info.map2stripe);
      object_placement.set_encode_type(m_encode_parameters.encodetype);
      object_placement.set_clientip(client_ip);
      object_placement.set_clientport(client_port);
      Stripe &t_stripe = m_stripe_table[object_info.map2stripe];
      std::unordered_set<int> t_cluster_set;
      for (int i = 0; i < int(t_stripe.blocks.size()); i++)
      {
        if (t_stripe.blocks[i]->map2key == key)
        {
          object_placement.add_datanodeip(m_node_table[t_stripe.blocks[i]->map2node].node_ip);
          object_placement.add_datanodeport(m_node_table[t_stripe.blocks[i]->map2node].node_port);
          object_placement.add_blockkeys(t_stripe.blocks[i]->block_key);
          t_cluster_set.insert(t_stripe.blocks[i]->map2cluster);
        }
      }
      // randomly select a cluster
      int idx = rand_num(int(t_cluster_set.size()));
      int r_cluster_id = *(std::next(t_cluster_set.begin(), idx));
      std::string chosen_proxy = m_cluster_table[r_cluster_id].proxy_ip + ":" + std::to_string(m_cluster_table[r_cluster_id].proxy_port);
      status = m_proxy_ptrs[chosen_proxy]->decodeAndGetObject(&decode_and_get, object_placement, &get_reply);
      if (status.ok())
      {
        std::cout << "[GET] getting value of " << key << std::endl;
      }
    }
    catch (std::exception &e)
    {
      std::cout << "getValue exception" << std::endl;
      std::cout << e.what() << std::endl;
    }
    return grpc::Status::OK;
  }

  grpc::Status CoordinatorImpl::delByKey(
      grpc::ServerContext *context,
      const coordinator_proto::KeyFromClient *del_key,
      coordinator_proto::RepIfDeling *delReplyClient)
  {
    try
    {
      std::string key = del_key->key();
      ObjectInfo object_info;
      m_mutex.lock();
      object_info = m_object_commit_table.at(key);
      m_object_updating_table[key] = m_object_commit_table[key];
      m_mutex.unlock();

      grpc::ClientContext context;
      proxy_proto::NodeAndBlock node_block;
      grpc::Status status;
      proxy_proto::DelReply del_reply;
      Stripe &t_stripe = m_stripe_table[object_info.map2stripe];
      std::unordered_set<int> t_cluster_set;
      for (int i = 0; i < int(t_stripe.blocks.size()); i++)
      {
        if (t_stripe.blocks[i]->map2key == key)
        {
          node_block.add_datanodeip(m_node_table[t_stripe.blocks[i]->map2node].node_ip);
          node_block.add_datanodeport(m_node_table[t_stripe.blocks[i]->map2node].node_port);
          node_block.add_blockkeys(t_stripe.blocks[i]->block_key);
          t_cluster_set.insert(t_stripe.blocks[i]->map2cluster);
        }
      }
      node_block.set_stripe_id(-1); // as a flag to distinguish delete key or stripe
      node_block.set_key(key);
      // randomly select a cluster
      int idx = rand_num(int(t_cluster_set.size()));
      int r_cluster_id = *(std::next(t_cluster_set.begin(), idx));
      std::string chosen_proxy = m_cluster_table[r_cluster_id].proxy_ip + ":" + std::to_string(m_cluster_table[r_cluster_id].proxy_port);
      status = m_proxy_ptrs[chosen_proxy]->deleteBlock(&context, node_block, &del_reply);
      delReplyClient->set_ifdeling(true);
      if (status.ok())
      {
        std::cout << "[DEL] deleting value of " << key << std::endl;
      }
    }
    catch (const std::exception &e)
    {
      std::cout << "deleteByKey exception" << std::endl;
      std::cout << e.what() << std::endl;
    }
    return grpc::Status::OK;
  }

  grpc::Status CoordinatorImpl::delByStripe(
      grpc::ServerContext *context,
      const coordinator_proto::StripeIdFromClient *stripeid,
      coordinator_proto::RepIfDeling *delReplyClient)
  {
    try
    {
      int t_stripe_id = stripeid->stripe_id();
      m_mutex.lock();
      m_stripe_deleting_table.push_back(t_stripe_id);
      m_mutex.unlock();

      grpc::ClientContext context;
      proxy_proto::NodeAndBlock node_block;
      grpc::Status status;
      proxy_proto::DelReply del_reply;
      Stripe &t_stripe = m_stripe_table[t_stripe_id];
      std::unordered_set<int> t_cluster_set;
      for (int i = 0; i < int(t_stripe.blocks.size()); i++)
      {
        if (t_stripe.blocks[i]->map2stripe == t_stripe_id)
        {
          node_block.add_datanodeip(m_node_table[t_stripe.blocks[i]->map2node].node_ip);
          node_block.add_datanodeport(m_node_table[t_stripe.blocks[i]->map2node].node_port);
          node_block.add_blockkeys(t_stripe.blocks[i]->block_key);
          t_cluster_set.insert(t_stripe.blocks[i]->map2cluster);
        }
      }
      node_block.set_stripe_id(t_stripe_id);
      node_block.set_key("");
      // randomly select a cluster
      int idx = rand_num(int(t_cluster_set.size()));
      int r_cluster_id = *(std::next(t_cluster_set.begin(), idx));
      std::string chosen_proxy = m_cluster_table[r_cluster_id].proxy_ip + ":" + std::to_string(m_cluster_table[r_cluster_id].proxy_port);
      status = m_proxy_ptrs[chosen_proxy]->deleteBlock(&context, node_block, &del_reply);
      delReplyClient->set_ifdeling(true);
      if (status.ok())
      {
        std::cout << "[DEL] deleting value of Stripe " << t_stripe_id << std::endl;
      }
    }
    catch (const std::exception &e)
    {
      std::cout << "deleteByStripe exception" << std::endl;
      std::cout << e.what() << std::endl;
    }
    return grpc::Status::OK;
  }

  grpc::Status CoordinatorImpl::listStripes(
      grpc::ServerContext *context,
      const coordinator_proto::RequestToCoordinator *req,
      coordinator_proto::RepStripeIds *listReplyClient)
  {
    try
    {
      for (auto it = m_stripe_table.begin(); it != m_stripe_table.end(); it++)
      {
        listReplyClient->add_stripe_ids(it->first);
      }
    }
    catch (const std::exception &e)
    {
      std::cerr << e.what() << '\n';
    }

    return grpc::Status::OK;
  }

  grpc::Status CoordinatorImpl::checkalive(
      grpc::ServerContext *context,
      const coordinator_proto::RequestToCoordinator *helloRequestToCoordinator,
      coordinator_proto::ReplyFromCoordinator *helloReplyFromCoordinator)
  {

    std::cout << "[Coordinator Check] alive " << helloRequestToCoordinator->name() << std::endl;
    return grpc::Status::OK;
  }
  grpc::Status CoordinatorImpl::reportCommitAbort(
      grpc::ServerContext *context,
      const coordinator_proto::CommitAbortKey *commit_abortkey,
      coordinator_proto::ReplyFromCoordinator *helloReplyFromCoordinator)
  {
    std::string key = commit_abortkey->key();
    ECProject::OpperateType opp = (ECProject::OpperateType)commit_abortkey->opp();
    int stripe_id = commit_abortkey->stripe_id();
    std::unique_lock<std::mutex> lck(m_mutex);
    try
    {
      if (commit_abortkey->ifcommitmetadata())
      {
        if (opp == SET)
        {
          m_object_commit_table[key] = m_object_updating_table[key];
          cv.notify_all();
          m_object_updating_table.erase(key);
        }
        else if (opp == DEL) // delete the metadata
        {
          if (stripe_id < 0) // delete key
          {
            if (IF_DEBUG)
            {
              std::cout << "[DEL] Proxy report delete key finish!" << std::endl;
            }
            ObjectInfo object_info = m_object_commit_table.at(key);
            stripe_id = object_info.map2stripe;
            m_object_commit_table.erase(key); // update commit table
            cv.notify_all();
            m_object_updating_table.erase(key);
            Stripe &t_stripe = m_stripe_table[stripe_id];
            std::vector<Block *>::iterator it1;
            for (it1 = t_stripe.blocks.begin(); it1 != t_stripe.blocks.end();)
            {
              if ((*it1)->map2key == key)
              {
                it1 = t_stripe.blocks.erase(it1);
              }
              else
              {
                it1++;
              }
            }
            if (t_stripe.blocks.empty()) // update stripe table
            {
              m_stripe_table.erase(stripe_id);
            }
            std::map<int, Cluster>::iterator it2; // update cluster table
            for (it2 = m_cluster_table.begin(); it2 != m_cluster_table.end(); it2++)
            {
              Cluster &t_cluster = it2->second;
              for (it1 = t_cluster.blocks.begin(); it1 != t_cluster.blocks.end();)
              {
                if ((*it1)->map2key == key)
                {
                  update_stripe_info_in_node(false, (*it1)->map2node, (*it1)->map2stripe); // update node table
                  it1 = t_cluster.blocks.erase(it1);
                }
                else
                {
                  it1++;
                }
              }
            }
          } // delete stripe
          else
          {
            if (IF_DEBUG)
            {
              std::cout << "[DEL] Proxy report delete stripe finish!" << std::endl;
            }
            auto its = std::find(m_stripe_deleting_table.begin(), m_stripe_deleting_table.end(), stripe_id);
            if (its != m_stripe_deleting_table.end())
            {
              m_stripe_deleting_table.erase(its);
            }
            cv.notify_all();
            // update stripe table
            m_stripe_table.erase(stripe_id);
            std::unordered_set<std::string> object_keys_set;
            // update cluster table
            std::map<int, Cluster>::iterator it2;
            for (it2 = m_cluster_table.begin(); it2 != m_cluster_table.end(); it2++)
            {
              Cluster &t_cluster = it2->second;
              for (auto it1 = t_cluster.blocks.begin(); it1 != t_cluster.blocks.end();)
              {
                if ((*it1)->map2stripe == stripe_id)
                {
                  object_keys_set.insert((*it1)->map2key);
                  it1 = t_cluster.blocks.erase(it1);
                }
                else
                {
                  it1++;
                }
              }
            }
            // update node table
            for (auto it3 = m_node_table.begin(); it3 != m_node_table.end(); it3++)
            {
              Node &t_node = it3->second;
              auto it4 = t_node.stripes.find(stripe_id);
              if (it4 != t_node.stripes.end())
              {
                t_node.stripes.erase(stripe_id);
              }
            }
            // update commit table
            for (auto it5 = object_keys_set.begin(); it5 != object_keys_set.end(); it5++)
            {
              auto it6 = m_object_commit_table.find(*it5);
              if (it6 != m_object_commit_table.end())
              {
                m_object_commit_table.erase(it6);
              }
            }
            // merge group
          }
          // if (IF_DEBUG)
          // {
          //   std::cout << "[DEL] Data placement after delete:" << std::endl;
          //   for (int i = 0; i < m_num_of_Clusters; i++)
          //   {
          //     Cluster &t_cluster = m_cluster_table[i];
          //     if (int(t_cluster.blocks.size()) > 0)
          //     {
          //       std::cout << "Cluster " << i << ": ";
          //       for (auto it = t_cluster.blocks.begin(); it != t_cluster.blocks.end(); it++)
          //       {
          //         std::cout << "[" << (*it)->block_key << ":S" << (*it)->map2stripe << "G" << (*it)->map2group << "N" << (*it)->map2node << "] ";
          //       }
          //       std::cout << std::endl;
          //     }
          //   }
          //   std::cout << std::endl;
          // }
        }
      }
      else
      {
        m_object_updating_table.erase(key);
      }
    }
    catch (std::exception &e)
    {
      std::cout << "reportCommitAbort exception" << std::endl;
      std::cout << e.what() << std::endl;
    }
    return grpc::Status::OK;
  }

  grpc::Status
  CoordinatorImpl::checkCommitAbort(grpc::ServerContext *context,
                                    const coordinator_proto::AskIfSuccess *key_opp,
                                    coordinator_proto::RepIfSuccess *reply)
  {
    std::unique_lock<std::mutex> lck(m_mutex);
    std::string key = key_opp->key();
    ECProject::OpperateType opp = (ECProject::OpperateType)key_opp->opp();
    int stripe_id = key_opp->stripe_id();
    if (opp == SET)
    {
      while (m_object_commit_table.find(key) == m_object_commit_table.end())
      {
        cv.wait(lck);
      }
    }
    else if (opp == DEL)
    {
      if (stripe_id < 0)
      {
        while (m_object_commit_table.find(key) != m_object_commit_table.end())
        {
          cv.wait(lck);
        }
      }
      else
      {
        auto it = std::find(m_stripe_deleting_table.begin(), m_stripe_deleting_table.end(), stripe_id);
        while (it != m_stripe_deleting_table.end())
        {
          cv.wait(lck);
          it = std::find(m_stripe_deleting_table.begin(), m_stripe_deleting_table.end(), stripe_id);
        }
      }
    }
    reply->set_ifcommit(true);
    return grpc::Status::OK;
  }

  bool CoordinatorImpl::init_proxyinfo()
  {
    for (auto cur = m_cluster_table.begin(); cur != m_cluster_table.end(); cur++)
    {
      std::string proxy_ip_and_port = cur->second.proxy_ip + ":" + std::to_string(cur->second.proxy_port);
      auto _stub = proxy_proto::proxyService::NewStub(grpc::CreateChannel(proxy_ip_and_port, grpc::InsecureChannelCredentials()));
      proxy_proto::CheckaliveCMD Cmd;
      proxy_proto::RequestResult result;
      grpc::ClientContext clientContext;
      Cmd.set_name("coordinator");
      grpc::Status status;
      status = _stub->checkalive(&clientContext, Cmd, &result);
      if (status.ok())
      {
        std::cout << "[Proxy Check] ok from " << proxy_ip_and_port << std::endl;
      }
      else
      {
        std::cout << "[Proxy Check] failed to connect " << proxy_ip_and_port << std::endl;
      }
      m_proxy_ptrs.insert(std::make_pair(proxy_ip_and_port, std::move(_stub)));
    }
    return true;
  }
  bool CoordinatorImpl::init_clusterinfo(std::string m_clusterinfo_path)
  {
    std::cout << "Cluster_information_path:" << m_clusterinfo_path << std::endl;
    tinyxml2::XMLDocument xml;
    xml.LoadFile(m_clusterinfo_path.c_str());
    tinyxml2::XMLElement *root = xml.RootElement();
    int node_id = 0;
    m_num_of_Clusters = 0;
    for (tinyxml2::XMLElement *cluster = root->FirstChildElement(); cluster != nullptr; cluster = cluster->NextSiblingElement())
    {
      std::string cluster_id(cluster->Attribute("id"));
      std::string proxy(cluster->Attribute("proxy"));
      std::cout << "cluster_id: " << cluster_id << " , proxy: " << proxy << std::endl;
      Cluster t_cluster;
      m_cluster_table[std::stoi(cluster_id)] = t_cluster;
      m_cluster_table[std::stoi(cluster_id)].cluster_id = std::stoi(cluster_id);
      auto pos = proxy.find(':');
      m_cluster_table[std::stoi(cluster_id)].proxy_ip = proxy.substr(0, pos);
      m_cluster_table[std::stoi(cluster_id)].proxy_port = std::stoi(proxy.substr(pos + 1, proxy.size()));
      for (tinyxml2::XMLElement *node = cluster->FirstChildElement()->FirstChildElement(); node != nullptr; node = node->NextSiblingElement())
      {
        std::string node_uri(node->Attribute("uri"));
        std::cout << "____node: " << node_uri << std::endl;
        m_cluster_table[std::stoi(cluster_id)].nodes.push_back(node_id);
        m_node_table[node_id].node_id = node_id;
        auto pos = node_uri.find(':');
        m_node_table[node_id].node_ip = node_uri.substr(0, pos);
        m_node_table[node_id].node_port = std::stoi(node_uri.substr(pos + 1, node_uri.size()));
        m_node_table[node_id].cluster_id = std::stoi(cluster_id);
        node_id++;
      }
      m_num_of_Clusters++;
    }
    return true;
  }

  int CoordinatorImpl::randomly_select_a_cluster(int stripe_id)
  {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dis_cluster(0, m_num_of_Clusters - 1);
    int r_cluster_id = dis_cluster(gen);
    while (m_cluster_table[r_cluster_id].stripes.find(stripe_id) != m_cluster_table[r_cluster_id].stripes.end())
    {
      r_cluster_id = dis_cluster(gen);
    }
    return r_cluster_id;
  }

  int CoordinatorImpl::randomly_select_a_node(int cluster_id, int stripe_id)
  {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dis_node(0, m_cluster_table[cluster_id].nodes.size() - 1);
    int r_node_id = m_cluster_table[cluster_id].nodes[dis_node(gen)];
    while (m_node_table[r_node_id].stripes.find(stripe_id) != m_node_table[r_node_id].stripes.end())
    {
      r_node_id = m_cluster_table[cluster_id].nodes[dis_node(gen)];
    }
    return r_node_id;
  }

  void CoordinatorImpl::update_stripe_info_in_node(bool add_or_sub, int t_node_id, int stripe_id)
  {
    int stripe_block_num = 1;
    if (m_node_table[t_node_id].stripes.find(stripe_id) != m_node_table[t_node_id].stripes.end())
    {
      stripe_block_num = m_node_table[t_node_id].stripes[stripe_id];
    }
    if (add_or_sub)
    {
      m_node_table[t_node_id].stripes[stripe_id] = stripe_block_num + 1;
    }
    else
    {
      if (stripe_block_num == 1)
      {
        m_node_table[t_node_id].stripes.erase(stripe_id);
      }
      else
      {
        m_node_table[t_node_id].stripes[stripe_id] = stripe_block_num - 1;
      }
    }
  }

  int CoordinatorImpl::generate_placement(int stripe_id, int block_size)
  {
    Stripe &stripe_info = m_stripe_table[stripe_id];
    int k = stripe_info.k;
    int l = stripe_info.l;
    int g_m = stripe_info.g_m;
    int b = m_encode_parameters.b_datapergroup;
    ECProject::EncodeType encode_type = m_encode_parameters.encodetype;
    ECProject::SingleStripePlacementType s_placement_type = m_encode_parameters.s_stripe_placementtype;
    ECProject::MultiStripesPlacementType m_placement_type = m_encode_parameters.m_stripe_placementtype;

    // generate stripe information
    int index = stripe_info.object_keys.size() - 1;
    std::string object_key = stripe_info.object_keys[index];
    Block *blocks_info = new Block[k + g_m + l];
    for (int i = 0; i < k + g_m + l; i++)
    {
      blocks_info[i].block_size = block_size;
      blocks_info[i].map2stripe = stripe_id;
      blocks_info[i].map2key = object_key;
      if (i < k)
      {
        std::string tmp = "_D";
        if (i < 10)
          tmp = "_D0";
        blocks_info[i].block_key = object_key + tmp + std::to_string(i);
        blocks_info[i].block_id = i;
        blocks_info[i].block_type = 'D';
        blocks_info[i].map2group = int(i / b);
        stripe_info.blocks.push_back(&blocks_info[i]);
      }
      else if (i >= k && i < k + g_m)
      {
        blocks_info[i].block_key = "Stripe" + std::to_string(stripe_id) + "_G" + std::to_string(i - k);
        blocks_info[i].block_type = 'G';
        blocks_info[i].map2group = l;
        stripe_info.blocks.push_back(&blocks_info[i]);
      }
      else
      {
        blocks_info[i].block_key = "Stripe" + std::to_string(stripe_id) + "_L" + std::to_string(i - k - g_m);
        blocks_info[i].block_type = 'L';
        blocks_info[i].map2group = i - k - g_m;
        stripe_info.blocks.push_back(&blocks_info[i]);
      }
    }

    if (encode_type == Azure_LRC || encode_type == Optimal_Cauchy_LRC)
    {
      if (s_placement_type == Optimal)
      {
        if (m_placement_type == Ran)
        {
          int idx = m_merge_groups.size() - 1;
          if (idx < 0 || int(m_merge_groups[idx].size()) == m_encode_parameters.x_stripepermergegroup)
          {
            std::vector<int> temp;
            temp.push_back(stripe_id);
            m_merge_groups.push_back(temp);
          }
          else
          {
            m_merge_groups[idx].push_back(stripe_id);
          }

          int g_cluster_id = -1;
          for (int i = 0; i < l; i++)
          {
            for (int j = i * b; j < (i + 1) * b; j += g_m + 1)
            {
              bool flag = false;
              if (j + g_m + 1 >= (i + 1) * b)
                flag = true;
              // randomly select a cluster
              int t_cluster_id = randomly_select_a_cluster(stripe_id);
              Cluster &t_cluster = m_cluster_table[t_cluster_id];
              // place every g+1 data blocks from each group to a single cluster
              for (int o = j; o < j + g_m + 1 && o < (i + 1) * b; o++)
              {
                // randomly select a node in the selected cluster
                int t_node_id = randomly_select_a_node(t_cluster_id, stripe_id);
                blocks_info[o].map2cluster = t_cluster_id;
                blocks_info[o].map2node = t_node_id;
                update_stripe_info_in_node(true, t_node_id, stripe_id);
                t_cluster.blocks.push_back(&blocks_info[o]);
                t_cluster.stripes.insert(stripe_id);
                stripe_info.place2clusters.insert(t_cluster_id);
              }
              // place local parity blocks
              if (flag)
              {
                if (j + g_m + 1 != (i + 1) * b) // b % (g + 1) != 0
                {
                  // randomly select a node in the selected cluster
                  int t_node_id = randomly_select_a_node(t_cluster_id, stripe_id);
                  blocks_info[k + g_m + i].map2cluster = t_cluster_id;
                  blocks_info[k + g_m + i].map2node = t_node_id;
                  update_stripe_info_in_node(true, t_node_id, stripe_id);
                  t_cluster.blocks.push_back(&blocks_info[k + g_m + i]);
                  t_cluster.stripes.insert(stripe_id);
                  stripe_info.place2clusters.insert(t_cluster_id);
                }
                else // place the local parity blocks together with global ones
                {
                  if (g_cluster_id == -1) // randomly select a new cluster
                  {
                    g_cluster_id = randomly_select_a_cluster(stripe_id);
                  }
                  Cluster &g_cluster = m_cluster_table[g_cluster_id];
                  int t_node_id = randomly_select_a_node(g_cluster_id, stripe_id);
                  blocks_info[k + g_m + i].map2cluster = g_cluster_id;
                  blocks_info[k + g_m + i].map2node = t_node_id;
                  update_stripe_info_in_node(true, t_node_id, stripe_id);
                  g_cluster.blocks.push_back(&blocks_info[k + g_m + i]);
                  g_cluster.stripes.insert(stripe_id);
                  stripe_info.place2clusters.insert(g_cluster_id);
                }
              }
            }
          }
          if (g_cluster_id == -1) // randomly select a new cluster
          {
            g_cluster_id = randomly_select_a_cluster(stripe_id);
          }
          Cluster &g_cluster = m_cluster_table[g_cluster_id];
          // place the global parity blocks to the selected cluster
          for (int i = 0; i < g_m; i++)
          {
            int t_node_id = randomly_select_a_node(g_cluster_id, stripe_id);
            blocks_info[k + i].map2cluster = g_cluster_id;
            blocks_info[k + i].map2node = t_node_id;
            update_stripe_info_in_node(true, t_node_id, stripe_id);
            g_cluster.blocks.push_back(&blocks_info[k + i]);
            g_cluster.stripes.insert(stripe_id);
            stripe_info.place2clusters.insert(g_cluster_id);
          }
        }
        else if (m_placement_type == DIS)
        {
          int required_cluster_num = ceil(b + 1, g_m + 1) * l + 1;
          int idx = m_merge_groups.size() - 1;
          if (b % (g_m + 1) == 0)
            required_cluster_num -= l;
          if (int(m_free_clusters.size()) < required_cluster_num || m_free_clusters.empty() || idx < 0 ||
              int(m_merge_groups[idx].size()) == m_encode_parameters.x_stripepermergegroup)
          {
            m_free_clusters.clear();
            m_free_clusters.shrink_to_fit();
            for (int i = 0; i < m_num_of_Clusters; i++)
            {
              m_free_clusters.push_back(i);
            }
            std::vector<int> temp;
            temp.push_back(stripe_id);
            m_merge_groups.push_back(temp);
          }
          else
          {
            m_merge_groups[idx].push_back(stripe_id);
          }

          int g_cluster_id = -1;
          for (int i = 0; i < l; i++)
          {
            for (int j = i * b; j < (i + 1) * b; j += g_m + 1)
            {
              bool flag = false;
              if (j + g_m + 1 >= (i + 1) * b)
                flag = true;
              // randomly select a cluster
              int t_cluster_id = m_free_clusters[rand_num(int(m_free_clusters.size()))];
              auto iter = std::find(m_free_clusters.begin(), m_free_clusters.end(), t_cluster_id);
              if (iter != m_free_clusters.end())
              {
                m_free_clusters.erase(iter);
              } // remove the selected cluster from the free list
              Cluster &t_cluster = m_cluster_table[t_cluster_id];
              // place every g+1 data blocks from each group to a single cluster
              for (int o = j; o < j + g_m + 1 && o < (i + 1) * b; o++)
              {
                // randomly select a node in the selected cluster
                int t_node_id = randomly_select_a_node(t_cluster_id, stripe_id);
                blocks_info[o].map2cluster = t_cluster_id;
                blocks_info[o].map2node = t_node_id;
                update_stripe_info_in_node(true, t_node_id, stripe_id);
                t_cluster.blocks.push_back(&blocks_info[o]);
                t_cluster.stripes.insert(stripe_id);
                stripe_info.place2clusters.insert(t_cluster_id);
              }
              // place local parity blocks
              if (flag)
              {
                if (j + g_m + 1 != (i + 1) * b) // b % (g + 1) != 0
                {
                  // randomly select a node in the selected cluster
                  int t_node_id = randomly_select_a_node(t_cluster_id, stripe_id);
                  blocks_info[k + g_m + i].map2cluster = t_cluster_id;
                  blocks_info[k + g_m + i].map2node = t_node_id;
                  update_stripe_info_in_node(true, t_node_id, stripe_id);
                  t_cluster.blocks.push_back(&blocks_info[k + g_m + i]);
                  t_cluster.stripes.insert(stripe_id);
                  stripe_info.place2clusters.insert(t_cluster_id);
                }
                else // place the local parity blocks together with global ones
                {
                  if (g_cluster_id == -1) // randomly select a new cluster
                  {
                    g_cluster_id = m_free_clusters[rand_num(int(m_free_clusters.size()))];
                    auto iter = std::find(m_free_clusters.begin(), m_free_clusters.end(), g_cluster_id);
                    if (iter != m_free_clusters.end())
                    {
                      m_free_clusters.erase(iter);
                    }
                  }
                  Cluster &g_cluster = m_cluster_table[g_cluster_id];
                  int t_node_id = randomly_select_a_node(g_cluster_id, stripe_id);
                  blocks_info[k + g_m + i].map2cluster = g_cluster_id;
                  blocks_info[k + g_m + i].map2node = t_node_id;
                  update_stripe_info_in_node(true, t_node_id, stripe_id);
                  g_cluster.blocks.push_back(&blocks_info[k + g_m + i]);
                  g_cluster.stripes.insert(stripe_id);
                  stripe_info.place2clusters.insert(g_cluster_id);
                }
              }
            }
          }
          if (g_cluster_id == -1) // randomly select a new cluster
          {
            g_cluster_id = m_free_clusters[rand_num(int(m_free_clusters.size()))];
            auto iter = std::find(m_free_clusters.begin(), m_free_clusters.end(), g_cluster_id);
            if (iter != m_free_clusters.end())
            {
              m_free_clusters.erase(iter);
            }
          }
          Cluster &g_cluster = m_cluster_table[g_cluster_id];
          // place the global parity blocks to the selected cluster
          for (int i = 0; i < g_m; i++)
          {
            int t_node_id = randomly_select_a_node(g_cluster_id, stripe_id);
            blocks_info[k + i].map2cluster = g_cluster_id;
            blocks_info[k + i].map2node = t_node_id;
            update_stripe_info_in_node(true, t_node_id, stripe_id);
            g_cluster.blocks.push_back(&blocks_info[k + i]);
            g_cluster.stripes.insert(stripe_id);
            stripe_info.place2clusters.insert(g_cluster_id);
          }
        }
        else if (m_placement_type == AGG)
        {
          int agg_clusters_num = ceil(b + 1, g_m + 1) * l + 1;
          if (b % (g_m + 1) == 0)
          {
            agg_clusters_num -= l;
          }
          int idx = m_merge_groups.size() - 1;
          if (idx < 0 || int(m_merge_groups[idx].size()) == m_encode_parameters.x_stripepermergegroup)
          {
            std::vector<int> temp;
            temp.push_back(stripe_id);
            m_merge_groups.push_back(temp);
            m_agg_start_cid = rand_num(m_num_of_Clusters - agg_clusters_num);
          }
          else
          {
            m_merge_groups[idx].push_back(stripe_id);
          }
          int t_cluster_id = m_agg_start_cid - 1;
          int g_cluster_id = -1;
          for (int i = 0; i < l; i++)
          {
            for (int j = i * b; j < (i + 1) * b; j += g_m + 1)
            {
              bool flag = false;
              if (j + g_m + 1 >= (i + 1) * b)
                flag = true;
              t_cluster_id += 1;
              Cluster &t_cluster = m_cluster_table[t_cluster_id];
              // place every g+1 data blocks from each group to a single cluster
              for (int o = j; o < j + g_m + 1 && o < (i + 1) * b; o++)
              {
                // randomly select a node in the selected cluster
                int t_node_id = randomly_select_a_node(t_cluster_id, stripe_id);
                blocks_info[o].map2cluster = t_cluster_id;
                blocks_info[o].map2node = t_node_id;
                update_stripe_info_in_node(true, t_node_id, stripe_id);
                t_cluster.blocks.push_back(&blocks_info[o]);
                t_cluster.stripes.insert(stripe_id);
                stripe_info.place2clusters.insert(t_cluster_id);
              }
              // place local parity blocks
              if (flag)
              {
                if (j + g_m + 1 != (i + 1) * b) // b % (g + 1) != 0
                {
                  // randomly select a node in the selected cluster
                  int t_node_id = randomly_select_a_node(t_cluster_id, stripe_id);
                  blocks_info[k + g_m + i].map2cluster = t_cluster_id;
                  blocks_info[k + g_m + i].map2node = t_node_id;
                  update_stripe_info_in_node(true, t_node_id, stripe_id);
                  t_cluster.blocks.push_back(&blocks_info[k + g_m + i]);
                  t_cluster.stripes.insert(stripe_id);
                  stripe_info.place2clusters.insert(t_cluster_id);
                }
                else // place the local parity blocks together with global ones
                {
                  if (g_cluster_id == -1)
                  {
                    g_cluster_id = t_cluster_id + 1;
                    t_cluster_id++;
                  }
                  Cluster &g_cluster = m_cluster_table[g_cluster_id];
                  int t_node_id = randomly_select_a_node(g_cluster_id, stripe_id);
                  blocks_info[k + g_m + i].map2cluster = g_cluster_id;
                  blocks_info[k + g_m + i].map2node = t_node_id;
                  update_stripe_info_in_node(true, t_node_id, stripe_id);
                  g_cluster.blocks.push_back(&blocks_info[k + g_m + i]);
                  g_cluster.stripes.insert(stripe_id);
                  stripe_info.place2clusters.insert(g_cluster_id);
                }
              }
            }
          }
          if (g_cluster_id == -1)
          {
            g_cluster_id = t_cluster_id + 1;
          }
          Cluster &g_cluster = m_cluster_table[g_cluster_id];
          // place the global parity blocks to the selected cluster
          for (int i = 0; i < g_m; i++)
          {
            int t_node_id = randomly_select_a_node(g_cluster_id, stripe_id);
            blocks_info[k + i].map2cluster = g_cluster_id;
            blocks_info[k + i].map2node = t_node_id;
            update_stripe_info_in_node(true, t_node_id, stripe_id);
            g_cluster.blocks.push_back(&blocks_info[k + i]);
            g_cluster.stripes.insert(stripe_id);
            stripe_info.place2clusters.insert(g_cluster_id);
          }
        }
        else if (m_placement_type == OPT)
        {
          int required_cluster_num = ceil(b + 1, g_m + 1) * l + 1;
          int agg_clusters_num = l + 1;
          if (b % (g_m + 1) == 0)
          {
            agg_clusters_num = 1;
            required_cluster_num -= l;
          }
          int idx = m_merge_groups.size() - 1;
          if (int(m_free_clusters.size()) < required_cluster_num - agg_clusters_num || m_free_clusters.empty() ||
              idx < 0 || int(m_merge_groups[idx].size()) == m_encode_parameters.x_stripepermergegroup)
          {
            m_agg_start_cid = rand_num(m_num_of_Clusters - agg_clusters_num);
            m_free_clusters.clear();
            m_free_clusters.shrink_to_fit();
            for (int i = 0; i < m_agg_start_cid; i++)
            {
              m_free_clusters.push_back(i);
            }
            for (int i = m_agg_start_cid + agg_clusters_num; i < m_num_of_Clusters; i++)
            {
              m_free_clusters.push_back(i);
            }
            std::vector<int> temp;
            temp.push_back(stripe_id);
            m_merge_groups.push_back(temp);
          }
          else
          {
            m_merge_groups[idx].push_back(stripe_id);
          }

          int agg_cluster_id = m_agg_start_cid - 1;
          int t_cluster_id = -1;
          int g_cluster_id = m_agg_start_cid + agg_clusters_num - 1;
          for (int i = 0; i < l; i++)
          {
            for (int j = i * b; j < (i + 1) * b; j += g_m + 1)
            {
              bool flag = false;
              if (j + g_m + 1 >= (i + 1) * b)
                flag = true;
              if (flag && j + g_m + 1 != (i + 1) * b)
              {
                t_cluster_id = ++agg_cluster_id;
              }
              else
              {
                t_cluster_id = m_free_clusters[rand_num(int(m_free_clusters.size()))];
                auto iter = std::find(m_free_clusters.begin(), m_free_clusters.end(), t_cluster_id);
                if (iter != m_free_clusters.end())
                {
                  m_free_clusters.erase(iter);
                }
              }
              Cluster &t_cluster = m_cluster_table[t_cluster_id];
              // place every g+1 data blocks from each group to a single cluster
              for (int o = j; o < j + g_m + 1 && o < (i + 1) * b; o++)
              {
                // randomly select a node in the selected cluster
                int t_node_id = randomly_select_a_node(t_cluster_id, stripe_id);
                blocks_info[o].map2cluster = t_cluster_id;
                blocks_info[o].map2node = t_node_id;
                update_stripe_info_in_node(true, t_node_id, stripe_id);
                t_cluster.blocks.push_back(&blocks_info[o]);
                t_cluster.stripes.insert(stripe_id);
                stripe_info.place2clusters.insert(t_cluster_id);
              }
              // place local parity blocks
              if (flag)
              {
                if (j + g_m + 1 != (i + 1) * b) // b % (g + 1) != 0
                {
                  // randomly select a node in the selected cluster
                  int t_node_id = randomly_select_a_node(t_cluster_id, stripe_id);
                  blocks_info[k + g_m + i].map2cluster = t_cluster_id;
                  blocks_info[k + g_m + i].map2node = t_node_id;
                  update_stripe_info_in_node(true, t_node_id, stripe_id);
                  t_cluster.blocks.push_back(&blocks_info[k + g_m + i]);
                  t_cluster.stripes.insert(stripe_id);
                  stripe_info.place2clusters.insert(t_cluster_id);
                }
                else // place the local parity blocks together with global ones
                {
                  Cluster &g_cluster = m_cluster_table[g_cluster_id];
                  int t_node_id = randomly_select_a_node(g_cluster_id, stripe_id);
                  blocks_info[k + g_m + i].map2cluster = g_cluster_id;
                  blocks_info[k + g_m + i].map2node = t_node_id;
                  update_stripe_info_in_node(true, t_node_id, stripe_id);
                  g_cluster.blocks.push_back(&blocks_info[k + g_m + i]);
                  g_cluster.stripes.insert(stripe_id);
                  stripe_info.place2clusters.insert(g_cluster_id);
                }
              }
            }
          }
          Cluster &g_cluster = m_cluster_table[g_cluster_id];
          // place the global parity blocks to the selected cluster
          for (int i = 0; i < g_m; i++)
          {
            int t_node_id = randomly_select_a_node(g_cluster_id, stripe_id);
            blocks_info[k + i].map2cluster = g_cluster_id;
            blocks_info[k + i].map2node = t_node_id;
            update_stripe_info_in_node(true, t_node_id, stripe_id);
            g_cluster.blocks.push_back(&blocks_info[k + i]);
            g_cluster.stripes.insert(stripe_id);
            stripe_info.place2clusters.insert(g_cluster_id);
          }
        }
      }
    }

    if (IF_DEBUG)
    {
      std::cout << std::endl;
      std::cout << "Data placement result:" << std::endl;
      for (int i = 0; i < m_num_of_Clusters; i++)
      {
        Cluster &t_cluster = m_cluster_table[i];
        if (int(t_cluster.blocks.size()) > 0)
        {
          std::cout << "Cluster " << i << ": ";
          for (auto it = t_cluster.blocks.begin(); it != t_cluster.blocks.end(); it++)
          {
            std::cout << "[" << (*it)->block_key << ":S" << (*it)->map2stripe << "G" << (*it)->map2group << "N" << (*it)->map2node << "] ";
          }
          std::cout << std::endl;
        }
      }
      std::cout << std::endl;
      std::cout << "Merge Group: ";
      for (auto it1 = m_merge_groups.begin(); it1 != m_merge_groups.end(); it1++)
      {
        std::cout << "[ ";
        for (auto it2 = (*it1).begin(); it2 != (*it1).end(); it2++)
        {
          std::cout << (*it2) << " ";
        }
        std::cout << "] ";
      }
      std::cout << std::endl;
    }

    // randomly select a cluster
    int r_idx = rand_num(int(stripe_info.place2clusters.size()));
    int selected_cluster_id = *(std::next(stripe_info.place2clusters.begin(), r_idx));
    if (IF_DEBUG)
    {
      std::cout << "[SET] Select the proxy in cluster " << selected_cluster_id << " to encode and set!" << std::endl;
    }
    return selected_cluster_id;
  }

  // merge
  grpc::Status CoordinatorImpl::requestMerge(
      grpc::ServerContext *context,
      const coordinator_proto::NumberOfStripesToMerge *numofstripe,
      coordinator_proto::RepIfMerged *mergeReplyClient)
  {
    int k = m_encode_parameters.k_datablock;
    int l = m_encode_parameters.l_localparityblock;
    int b = m_encode_parameters.b_datapergroup;
    int g_m = m_encode_parameters.g_m_globalparityblock;
    int m = b % (g_m + 1);
    EncodeType encodetype = m_encode_parameters.encodetype;
    int num_of_stripes = numofstripe->num_of_stripes();
    int total_stripes = m_stripe_table.size();
    if (total_stripes % num_of_stripes != 0 ||
        (encodetype == Azure_LRC && m_merge_degree == 0 && m != 0 && m != g_m && num_of_stripes != g_m / m) ||
        (encodetype == Optimal_Cauchy_LRC && m_merge_degree == 0 && m != 0 && m != g_m && num_of_stripes != g_m / m))
    {
      mergeReplyClient->set_ifmerged(false);
      return grpc::Status::OK;
    }
    MultiStripesPlacementType m_s_placementtype = m_encode_parameters.m_stripe_placementtype;
    if (m_s_placementtype == DIS || m_s_placementtype == OPT)
    {
      int num_stripepergroup = m_merge_groups[0].size();
      if (num_stripepergroup % num_of_stripes != 0)
      {
        mergeReplyClient->set_ifmerged(false);
        return grpc::Status::OK;
      }
    }
    int tot_stripe_num = int(m_stripe_table.size());
    int stripe_cnt = 0;
    double t_lc = 0.0;
    double t_gc = 0.0;
    double t_dc = 0.0;
    std::vector<std::vector<int>>::iterator it_g;
    std::vector<int>::iterator it_s, it_t;
    std::vector<std::vector<int>> new_merge_groups;
    for (it_g = m_merge_groups.begin(); it_g != m_merge_groups.end(); it_g++)
    {
      int cur_block_id = 0;
      std::vector<int> s_merge_group;
      // for each xi stripes
      for (it_s = (*it_g).begin(); it_s != (*it_g).end(); it_s += num_of_stripes)
      {
        int l_stripe_id = m_cur_stripe_id;
        int l_cluster_id[l];
        int g_cluster_id;
        std::vector<int> l_node_id;
        std::vector<int> g_node_id;
        std::unordered_set<int> old_stripe_id_set;

        // for request
        std::map<int, proxy_proto::locationInfo> block_location;
        proxy_proto::mainRecalPlan l_recal_plan[l];
        proxy_proto::mainRecalPlan g_main_plan;
        std::map<int, proxy_proto::locationInfo> parity_location[l];
        proxy_proto::mainRecalPlan l_main_plan[l];
        proxy_proto::NodeAndBlock old_parities;
        std::unordered_set<int> old_parities_cluster_set;

        int block_size;
        // merge and generate new stripe information
        Stripe larger_stripe;
        larger_stripe.stripe_id = l_stripe_id;
        larger_stripe.l = m_encode_parameters.l_localparityblock;
        larger_stripe.g_m = m_encode_parameters.g_m_globalparityblock;
        // for each stripe
        for (it_t = it_s; it_t != it_s + num_of_stripes; it_t++)
        {
          int t_stripe_id = *(it_t);
          old_stripe_id_set.insert(t_stripe_id);
          Stripe &t_stripe = m_stripe_table[t_stripe_id];
          larger_stripe.k = t_stripe.k * num_of_stripes;
          larger_stripe.object_keys.insert(larger_stripe.object_keys.end(), t_stripe.object_keys.begin(), t_stripe.object_keys.end());
          larger_stripe.object_sizes.insert(larger_stripe.object_sizes.end(), t_stripe.object_sizes.begin(), t_stripe.object_sizes.end());
          std::vector<Block *>::iterator it_b, it_c;
          // for each block
          for (it_b = t_stripe.blocks.begin(); it_b != t_stripe.blocks.end(); it_b++)
          {
            Block *t_block = *it_b;
            t_block->map2stripe = l_stripe_id;
            update_stripe_info_in_node(false, t_block->map2node, t_block->map2stripe);
            m_cluster_table[t_block->map2cluster].stripes.erase(t_block->map2stripe);
            if (t_block->block_type == 'D')
            {
              int t_cluster_id = t_block->map2cluster;
              t_block->block_id = cur_block_id++;
              larger_stripe.blocks.push_back(t_block);
              // for global parity block recalculation, find out the location of each data block
              if (block_location.find(t_cluster_id) == block_location.end())
              {
                Cluster &t_cluster = m_cluster_table[t_cluster_id];
                proxy_proto::locationInfo new_location;
                new_location.set_cluster_id(t_cluster_id);
                new_location.set_proxy_ip(t_cluster.proxy_ip);
                new_location.set_proxy_port(t_cluster.proxy_port);
                block_location[t_cluster_id] = new_location;
              }
              int t_node_id = t_block->map2node;
              Node &t_node = m_node_table[t_node_id];
              proxy_proto::locationInfo &t_location = block_location[t_cluster_id];
              t_location.add_datanodeip(t_node.node_ip);
              t_location.add_datanodeport(t_node.node_port);
              t_location.add_blockkeys(t_block->block_key);
              t_location.add_blockids(t_block->block_id);
              update_stripe_info_in_node(true, t_block->map2node, l_stripe_id);
              m_cluster_table[t_block->map2cluster].stripes.insert(l_stripe_id);
            }
            else if (t_block->block_type == 'L')
            {
              int t_cluster_id = t_block->map2cluster;
              int t_gid = t_block->map2group;
              l_cluster_id[t_gid] = t_block->map2cluster;
              Node &l_node = m_node_table[t_block->map2node];
              l_node_id.push_back(t_block->map2node);
              l_recal_plan[t_gid].add_p_datanodeip(l_node.node_ip);
              l_recal_plan[t_gid].add_p_datanodeport(l_node.node_port);
              l_recal_plan[t_gid].add_p_blockkeys(t_block->block_key);
              // for local parity block recalculation, find out the location of old local parities
              if (parity_location[t_gid].find(t_cluster_id) == parity_location[t_gid].end())
              {
                Cluster &t_cluster = m_cluster_table[t_cluster_id];
                proxy_proto::locationInfo new_location;
                new_location.set_cluster_id(t_cluster_id);
                new_location.set_proxy_ip(t_cluster.proxy_ip);
                new_location.set_proxy_port(t_cluster.proxy_port);
                parity_location[t_gid][t_cluster_id] = new_location;
              }
              proxy_proto::locationInfo &t_location = parity_location[t_gid][t_cluster_id];
              t_location.add_datanodeip(l_node.node_ip);
              t_location.add_datanodeport(l_node.node_port);
              t_location.add_blockkeys(t_block->block_key);
              // remove the old local parity block from the cluster
              Cluster &l_cluster = m_cluster_table[t_block->map2cluster];
              for (it_c = l_cluster.blocks.begin(); it_c != l_cluster.blocks.end(); it_c++)
              {
                if ((*it_c)->block_key == t_block->block_key)
                {
                  l_cluster.blocks.erase(it_c);
                  break;
                }
              }
              // for delete
              old_parities.add_datanodeip(l_node.node_ip);
              old_parities.add_datanodeport(l_node.node_port);
              old_parities.add_blockkeys(t_block->block_key);
              old_parities_cluster_set.insert(t_block->map2cluster);
            }
            else if (t_block->block_type == 'G')
            {
              // for global parity block recalculation
              g_cluster_id = t_block->map2cluster;
              Node &g_node = m_node_table[t_block->map2node];
              g_node_id.push_back(t_block->map2node);
              // g_main_plan.add_p_datanodeip(g_node.node_ip);
              // g_main_plan.add_p_datanodeport(g_node.node_port);
              // g_main_plan.add_p_blockkeys(t_block->block_key);
              // for local parity block recalculation of Optimal Cauchy LRC
              // find out the location of old global parities
              if (encodetype == Optimal_Cauchy_LRC){
                for (int t_gid = 0; t_gid < l; t_gid++){
                  if (parity_location[t_gid].find(g_cluster_id) == parity_location[t_gid].end())
                  {
                    Cluster &t_cluster = m_cluster_table[g_cluster_id];
                    proxy_proto::locationInfo new_location;
                    new_location.set_cluster_id(g_cluster_id);
                    new_location.set_proxy_ip(t_cluster.proxy_ip);
                    new_location.set_proxy_port(t_cluster.proxy_port);
                    parity_location[t_gid][g_cluster_id] = new_location;
                  }
                  proxy_proto::locationInfo &t_location = parity_location[t_gid][g_cluster_id];
                  t_location.add_datanodeip(g_node.node_ip);
                  t_location.add_datanodeport(g_node.node_port);
                  t_location.add_blockkeys(t_block->block_key);
                }
              }
              // remove the old global parity block from the cluster
              Cluster &g_cluster = m_cluster_table[t_block->map2cluster];
              for (it_c = g_cluster.blocks.begin(); it_c != g_cluster.blocks.end(); it_c++)
              {
                if ((*it_c)->block_key == t_block->block_key)
                {
                  g_cluster.blocks.erase(it_c);
                  break;
                }
              }
              // for delete
              old_parities.add_datanodeip(g_node.node_ip);
              old_parities.add_datanodeport(g_node.node_port);
              old_parities.add_blockkeys(t_block->block_key);
              old_parities_cluster_set.insert(t_block->map2cluster);
            }
            block_size = t_block->block_size;
          }
          larger_stripe.place2clusters.insert(t_stripe.place2clusters.begin(), t_stripe.place2clusters.end());
        }
        if (IF_DEBUG)
        {
          std::cout << std::endl;
          std::cout << "l_cluster_id : ";
          for (int i = 0; i < l; i++)
          {
            std::cout << l_cluster_id[i] << " ";
          }
          std::cout << std::endl;
          std::cout << "l_node_id : ";
          for (int i = 0; i < int(l_node_id.size()); i++)
          {
            std::cout << l_node_id[i] << " ";
          }
          std::cout << std::endl;
          std::cout << "g_cluster_id : " << g_cluster_id << std::endl;
          std::cout << "g_node_id : ";
          for (int i = 0; i < int(g_node_id.size()); i++)
          {
            std::cout << g_node_id[i] << " ";
          }
          std::cout << std::endl;
        }
        if (IF_DEBUG)
        {
          std::cout << "\033[1;33m[MERGE] Select cluster and node to place new parity blocks:\033[0m" << std::endl;
        }
        // generate new parity block
        for (int i = 0; i < l; i++)
        {
          std::string t_block_key = "Stripe" + std::to_string(l_stripe_id) + "_L" + std::to_string(i);
          int t_map2cluster = l_cluster_id[i];
          int t_map2node = l_node_id[l * (num_of_stripes - 1) + i];
          Block *t_block = new Block(t_block_key, 'L', block_size, i, l_stripe_id, t_map2cluster, t_map2node, "");
          if (IF_DEBUG)
          {
            std::cout << "\033[1;33m" << t_block->block_key << ": Cluster" << t_block->map2cluster << ", Node" << t_block->map2node << "\033[0m" << std::endl;
          }
          larger_stripe.blocks.push_back(t_block);
          update_stripe_info_in_node(true, t_map2node, l_stripe_id);
          m_cluster_table[t_map2cluster].stripes.insert(l_stripe_id);
          Cluster &t_cluster = m_cluster_table[t_map2cluster];
          t_cluster.blocks.push_back(t_block);
          auto it = std::find(t_cluster.nodes.begin(), t_cluster.nodes.end(), t_map2node);
          if (it == t_cluster.nodes.end())
          {
            std::cout << "[Generate new parity block] the selected node not in the selected cluster!" << std::endl;
          }
          // for local parity block recalculation, the location of the new parities
          Node &l_node = m_node_table[t_map2node];
          l_main_plan[i].add_p_datanodeip(l_node.node_ip);
          l_main_plan[i].add_p_datanodeport(l_node.node_port);
          l_main_plan[i].add_p_blockkeys(t_block_key);
        }
        for (int i = 0; i < g_m; i++)
        {
          std::string t_block_key = "Stripe" + std::to_string(l_stripe_id) + "_G" + std::to_string(i);
          int t_map2node = g_node_id[g_m * (num_of_stripes - 1) + i];
          Block *t_block = new Block(t_block_key, 'G', block_size, l, l_stripe_id, g_cluster_id, t_map2node, "");
          if (IF_DEBUG)
          {
            std::cout << "\033[1;33m" << t_block->block_key << ": Cluster" << t_block->map2cluster << ", Node" << t_block->map2node << "\033[0m" << std::endl;
          }
          larger_stripe.blocks.push_back(t_block);
          update_stripe_info_in_node(true, t_map2node, l_stripe_id);
          m_cluster_table[g_cluster_id].stripes.insert(l_stripe_id);
          Cluster &t_cluster = m_cluster_table[g_cluster_id];
          t_cluster.blocks.push_back(t_block);
          auto it = std::find(t_cluster.nodes.begin(), t_cluster.nodes.end(), t_map2node);
          if (it == t_cluster.nodes.end())
          {
            std::cout << "[Generate new parity block] the selected node not in the selected cluster!" << std::endl;
          }
          // for global parity block recalculation, the location of the new parities
          Node &g_node = m_node_table[t_map2node];
          g_main_plan.add_p_datanodeip(g_node.node_ip);
          g_main_plan.add_p_datanodeport(g_node.node_port);
          g_main_plan.add_p_blockkeys(t_block_key);
          // for local parity block recalculation of Optimal Cauchy LRC, derive new global parities
          if (encodetype == Optimal_Cauchy_LRC){
            for (int t_gid = 0; t_gid < l; t_gid++){
              proxy_proto::locationInfo &t_location = parity_location[t_gid][g_cluster_id];
              t_location.add_datanodeip(g_node.node_ip);
              t_location.add_datanodeport(g_node.node_port);
              t_location.add_blockkeys(t_block_key);
            }
          }
        }

        if (IF_DEBUG)
        {
          // print the result
          std::cout << std::endl;
          std::cout << "Data placement before data block relocation:" << std::endl;
          for (int i = 0; i < m_num_of_Clusters; i++)
          {
            Cluster &t_cluster = m_cluster_table[i];
            if (int(t_cluster.blocks.size()) > 0)
            {
              std::cout << "Cluster " << i << ": ";
              for (auto it = t_cluster.blocks.begin(); it != t_cluster.blocks.end(); it++)
              {
                std::cout << "[" << (*it)->block_key << ":S" << (*it)->map2stripe << "G" << (*it)->map2group << "N" << (*it)->map2node << "] ";
              }
              std::cout << std::endl;
            }
          }
          std::cout << std::endl;
        }

        // find out the data blocks to relocate
        int num2mov_v = 0, num2mov_k = 0;
        std::unordered_set<int>::iterator it;
        std::vector<std::string> block_to_move_key;
        std::vector<int> block_src_node;
        std::vector<int> block_des_node;
        std::unordered_set<int> new_cluster_set;
        // the blocks that voilate single-cluster fault tolerance
        for (it = larger_stripe.place2clusters.begin(); it != larger_stripe.place2clusters.end(); it++)
        {
          std::map<char, std::vector<ECProject::Block *>> block_info;
          int t_cluster_id = *it;
          Cluster &t_cluster = m_cluster_table[t_cluster_id];
          blocks_in_cluster(block_info, t_cluster_id, l_stripe_id);
          int nt = block_info['T'].size(); // num of blocks from the xi stripes in the cluster
          int nd = block_info['D'].size(); // num of data blocks
          int nl = block_info['L'].size(); // num of local parity blocks
          int ng = block_info['G'].size(); // num of global parity blocks
          if (IF_DEBUG)
          {
            std::cout << "\033[1;31m";
            std::cout << "(Blocks number)Cluster" << t_cluster_id << ",Stripe" << l_stripe_id << ": total-" << nt << " data-" << nd << " local-" << nl << " global-" << ng;
            std::cout << "\033[0m" << std::endl;
          }
          int max_group_id = -1;
          int max_group_num = 0;
          find_max_group(max_group_id, max_group_num, t_cluster_id, l_stripe_id);
          std::vector<ECProject::Block *> block_to_move;
          std::vector<ECProject::Block *>::iterator it_b, it_c;
          int num_to_move = 0;
          if (ng > 0 && nd > 0) // move the other blocks except global parity blocks, when there is any data block
          {
            if (IF_DEBUG)
            {
              std::cout << "\033[1;35mCase 1\033[0m\n";
            }
            for (it_b = block_info['D'].begin(); it_b != block_info['D'].end(); it_b++)
            {
              block_to_move.push_back(*it_b);
            }
            for (it_b = block_info['L'].begin(); it_b != block_info['L'].end(); it_b++)
            {
              block_to_move.push_back(*it_b);
            }
            num_to_move = nd + nl;
          }
          else if (nt > g_m + 1 && nd > 0) // remain the blocks from the group with maximum number of blocks in the cluster, but the remaining number can't exceed g+1
          {
            if (IF_DEBUG)
            {
              std::cout << "\033[1;35mCase 2\033[0m\n";
            }
            int m_cnt = 0;
            if (max_group_num >= g_m + 1)
            {
              num_to_move = nt - g_m - 1;
              m_cnt = max_group_num - g_m - 1;
            }
            else
            {
              num_to_move = nt - max_group_num;
            }
            for (it_b = block_info['T'].begin(); it_b != block_info['T'].end(); it_b++)
            {
              if ((*it_b)->map2group != max_group_id)
              {
                block_to_move.push_back(*it_b);
              }
              else if (m_cnt && (*it_b)->map2group == max_group_id)
              {
                block_to_move.push_back(*it_b);
                m_cnt--;
              }
            }
          }
          else if (nt != max_group_num && nd > 0) //// remain the blocks from the group with maximum number of blocks in the cluster
          {
            if (IF_DEBUG)
            {
              std::cout << "\033[1;35mCase 3\033[0m\n";
            }
            num_to_move = nt - max_group_num;
            for (it_b = block_info['T'].begin(); it_b != block_info['T'].end(); it_b++)
            {
              if (int((*it_b)->map2group) != max_group_id)
              {
                block_to_move.push_back(*it_b);
              }
            }
          }
          if (num_to_move != int(block_to_move.size()))
          {
            std::cout << "[MERGE] Error! number of blocks to move not matches!" << std::endl;
          }
          // find destination cluster and node for each block
          for (it_b = block_to_move.begin(); it_b != block_to_move.end(); it_b++)
          {
            block_to_move_key.push_back((*it_b)->block_key);
            block_src_node.push_back((*it_b)->map2node);
            bool flag_m = false;
            std::unordered_set<int>::iterator it_a;
            for (it_a = larger_stripe.place2clusters.begin(); it_a != larger_stripe.place2clusters.end(); it_a++)
            {
              int t_cid = *it_a;
              if (t_cid != t_cluster_id)
              {
                max_group_id = -1;
                max_group_num = 0;
                int block_num = count_block_num('T', t_cid, l_stripe_id, -1);
                find_max_group(max_group_id, max_group_num, t_cid, l_stripe_id);
                if (!find_block('G', t_cid, l_stripe_id) && 0 < block_num && block_num < g_m + 1 && int((*it_b)->map2group) == max_group_id)
                {
                  update_stripe_info_in_node(false, (*it_b)->map2node, (*it_b)->map2stripe);
                  int r_node_id = randomly_select_a_node(t_cid, l_stripe_id);
                  (*it_b)->map2cluster = t_cid;
                  (*it_b)->map2node = r_node_id;
                  for (it_c = t_cluster.blocks.begin(); it_c != t_cluster.blocks.end(); it_c++)
                  {
                    if ((*it_c)->block_key == (*it_b)->block_key)
                    {
                      t_cluster.blocks.erase(it_c);
                      break;
                    }
                  }
                  Cluster &t_cluster_des = m_cluster_table[t_cid];
                  t_cluster_des.blocks.push_back((*it_b));
                  block_des_node.push_back((*it_b)->map2node);
                  update_stripe_info_in_node(true, (*it_b)->map2node, l_stripe_id);
                  m_cluster_table[(*it_b)->map2cluster].stripes.insert(l_stripe_id);
                  flag_m = true;
                  break;
                }
              }
            }
            if (!flag_m)
            {
              for (it_a = new_cluster_set.begin(); it_a != new_cluster_set.end(); it_a++)
              {
                int t_cid = *it_a;
                if (t_cid != t_cluster_id)
                {
                  max_group_id = -1;
                  max_group_num = 0;
                  int block_num = count_block_num('T', t_cid, l_stripe_id, -1);
                  find_max_group(max_group_id, max_group_num, t_cid, l_stripe_id);
                  if (!find_block('G', t_cid, l_stripe_id) && block_num > 0 && block_num < g_m + 1 && (*it_b)->map2group == max_group_id)
                  {
                    update_stripe_info_in_node(false, (*it_b)->map2node, (*it_b)->map2stripe);
                    int r_node_id = randomly_select_a_node(t_cid, l_stripe_id);
                    (*it_b)->map2cluster = t_cid;
                    (*it_b)->map2node = r_node_id;
                    for (it_c = t_cluster.blocks.begin(); it_c != t_cluster.blocks.end(); it_c++)
                    {
                      if ((*it_c)->block_key == (*it_b)->block_key)
                      {
                        t_cluster.blocks.erase(it_c);
                        break;
                      }
                    }
                    Cluster &t_cluster_des = m_cluster_table[t_cid];
                    t_cluster_des.blocks.push_back((*it_b));
                    block_des_node.push_back((*it_b)->map2node);
                    update_stripe_info_in_node(true, (*it_b)->map2node, l_stripe_id);
                    m_cluster_table[(*it_b)->map2cluster].stripes.insert(l_stripe_id);
                    flag_m = true;
                    break;
                  }
                }
              }
            }
            if (!flag_m) // select an new cluster and place into it
            {
              int t_cid = rand_num(m_num_of_Clusters);
              while (count_block_num('T', t_cid, l_stripe_id, -1) > 0)
              {
                t_cid = rand_num(m_num_of_Clusters);
              }
              update_stripe_info_in_node(false, (*it_b)->map2node, (*it_b)->map2stripe);
              int r_node_id = randomly_select_a_node(t_cid, l_stripe_id);
              (*it_b)->map2cluster = t_cid;
              (*it_b)->map2node = r_node_id;
              for (it_c = t_cluster.blocks.begin(); it_c != t_cluster.blocks.end(); it_c++)
              {
                if ((*it_c)->block_key == (*it_b)->block_key)
                {
                  t_cluster.blocks.erase(it_c);
                  break;
                }
              }
              Cluster &t_cluster_des = m_cluster_table[t_cid];
              t_cluster_des.blocks.push_back((*it_b));
              block_des_node.push_back((*it_b)->map2node);
              update_stripe_info_in_node(true, (*it_b)->map2node, l_stripe_id);
              m_cluster_table[(*it_b)->map2cluster].stripes.insert(l_stripe_id);
              flag_m = true;
              new_cluster_set.insert(t_cid);
            }
            if (!flag_m)
            {
              std::cout << "[MERGE] reloc1 : can't find out a des-cluster to move block " << (*it_b)->block_key << std::endl;
            }
          }
        }
        if (IF_DEBUG)
        {
          std::cout << "[MERGE] voilated blocks to relocate:";
          for (int ii = 0; ii < int(block_to_move_key.size()); ii++)
          {
            std::cout << block_to_move_key[ii] << "[" << block_src_node[ii] << "->" << block_des_node[ii] << "] ";
          }
          std::cout << std::endl;
        }
        larger_stripe.place2clusters.insert(new_cluster_set.begin(), new_cluster_set.end());
        num2mov_v = int(block_to_move_key.size());

        // to keep each locap group placed in fewest cluster
        int bi = larger_stripe.k / larger_stripe.l;
        int c_a = ceil(bi + 1, g_m + 1);
        for (int i = 0; i < larger_stripe.l; i++) // for each local group
        {
          int c_b = 0;
          std::vector<int> block_from_group_in_cluster;
          // std::cout << "\033[1;31m";
          for (int j = 0; j < m_num_of_Clusters; j++)
          {
            int b_cnt = count_block_num('T', j, l_stripe_id, i);
            if (b_cnt > 0)
              c_b++;
            block_from_group_in_cluster.push_back(b_cnt);
            // std::cout << j << ":" << b_cnt << " ";
          }
          // std::cout << "\033[0m" << std::endl;
          if (c_b > c_a)
          {
            if (IF_DEBUG)
            {
              std::cout << "\033[1;31m[MERGE] Group " << i << " Cluster number: actual-" << c_b << " expected-" << c_a << "\033[0m" << std::endl;
            }
            int c_m = c_b - c_a;
            auto idxs = argsort(block_from_group_in_cluster);
            std::vector<int> del_cluster;
            int c_cnt = 0;
            int idx = 0;
            while (c_cnt < c_m)
            {
              if (block_from_group_in_cluster[idxs[idx]] > 0)
              {
                del_cluster.push_back(idxs[idx]);
                c_cnt++;
              }
              idx++;
            }
            std::vector<ECProject::Block *> block_to_move;
            std::vector<ECProject::Block *>::iterator it_b, it_c;
            for (int j = 0; j < int(del_cluster.size()); j++)
            {
              int t_cluster_id = del_cluster[j];
              Cluster &t_cluster = m_cluster_table[t_cluster_id];
              for (it_b = t_cluster.blocks.begin(); it_b != t_cluster.blocks.end(); it_b++)
              {
                if ((*it_b)->map2stripe == l_stripe_id && int((*it_b)->map2group) == i)
                {
                  block_to_move.push_back((*it_b));
                }
              }
            }
            // find destination cluster and node for each moved block
            for (it_b = block_to_move.begin(); it_b != block_to_move.end(); it_b++)
            {
              int t_cluster_id = (*it_b)->map2cluster;
              Cluster &t_cluster = m_cluster_table[t_cluster_id];
              block_to_move_key.push_back((*it_b)->block_key);
              block_src_node.push_back((*it_b)->map2node);
              bool flag_m = false;
              std::unordered_set<int>::iterator it_a;
              for (it_a = larger_stripe.place2clusters.begin(); it_a != larger_stripe.place2clusters.end(); it_a++)
              {
                int t_cid = *it_a;
                if (t_cid != t_cluster_id)
                {
                  int max_group_id = -1, max_group_num = 0;
                  int block_num = count_block_num('T', t_cid, l_stripe_id, -1);
                  find_max_group(max_group_id, max_group_num, t_cid, l_stripe_id);
                  if (!find_block('G', t_cid, l_stripe_id) && block_num > 0 && block_num < g_m + 1 && (*it_b)->map2group == max_group_id)
                  {
                    update_stripe_info_in_node(false, (*it_b)->map2node, (*it_b)->map2stripe);
                    int r_node_id = randomly_select_a_node(t_cid, l_stripe_id);
                    (*it_b)->map2cluster = t_cid;
                    (*it_b)->map2node = r_node_id;
                    for (it_c = t_cluster.blocks.begin(); it_c != t_cluster.blocks.end(); it_c++)
                    {
                      if ((*it_c)->block_key == (*it_b)->block_key)
                      {
                        t_cluster.blocks.erase(it_c);
                        break;
                      }
                    }
                    Cluster &t_cluster_des = m_cluster_table[t_cid];
                    t_cluster_des.blocks.push_back((*it_b));
                    block_des_node.push_back((*it_b)->map2node);
                    update_stripe_info_in_node(true, (*it_b)->map2node, l_stripe_id);
                    m_cluster_table[(*it_b)->map2cluster].stripes.insert(l_stripe_id);
                    flag_m = true;
                    break;
                  }
                }
              }
              if (!flag_m)
              {
                std::cout << "[MERGE] reloc2 : can't find out a des-cluster to move block " << (*it_b)->block_key << std::endl;
              }
            }
          }
        }
        if (IF_DEBUG)
        {
          std::cout << "[MERGE] all blocks to relocate:";
          for (int ii = 0; ii < int(block_to_move_key.size()); ii++)
          {
            std::cout << block_to_move_key[ii] << "[" << block_src_node[ii] << "->" << block_des_node[ii] << "] ";
          }
          std::cout << std::endl;
        }
        num2mov_k = int(block_to_move_key.size()) - num2mov_v;

        // remove the 'empty' cluster from the set
        // std::vector<int> empty_clusters;
        std::unordered_set<int>::iterator it_a;
        if (IF_DEBUG)
        {
          std::cout << "[MERGE] Clusters that places Stripe " << l_stripe_id << ":";
        }
        for (it_a = larger_stripe.place2clusters.begin(); it_a != larger_stripe.place2clusters.end();)
        {
          int t_cid = *it_a;
          if (IF_DEBUG)
          {
            std::cout << " " << t_cid;
          }
          if (t_cid >= 0 && count_block_num('T', t_cid, l_stripe_id, -1) == 0)
          {
            // empty_clusters.push_back(t_cid);
            it_a = larger_stripe.place2clusters.erase(it_a);
            if (IF_DEBUG)
            {
              std::cout << "(remove)";
            }
          }
          else
          {
            it_a++;
          }
        }
        if (IF_DEBUG)
        {
          std::cout << std::endl;
        }
        // for (int i = 0; i < int(empty_clusters.size()); i++)
        // {
        //   auto it_e = larger_stripe.place2clusters.find(empty_clusters[i]);
        //   if (it_e != larger_stripe.place2clusters.end())
        //   {
        //     larger_stripe.place2clusters.erase(it_e);
        //   }
        // }

        // time
        double temp_time = 0.0;
        struct timeval l_start_time, l_end_time;
        struct timeval g_start_time, g_end_time;
        struct timeval d_start_time, d_end_time;

        if (IF_DEBUG)
        {
          std::cout << "[MERGE] Start to recalculate global parity blocks for Stripe" << l_stripe_id << std::endl;
        }
        temp_time = 0.0;
        gettimeofday(&g_start_time, NULL);
        // global parity block recalculation
        auto send_main_plan = [this, k, l, g_m, block_size, g_main_plan, block_location, g_cluster_id, l_stripe_id]() mutable
        {
          // main
          g_main_plan.set_type(true);
          g_main_plan.set_k(k);
          g_main_plan.set_l(l);
          g_main_plan.set_g_m(g_m);
          g_main_plan.set_block_size(block_size);
          g_main_plan.set_if_partial_decoding(m_encode_parameters.partial_decoding);
          g_main_plan.set_stripe_id(l_stripe_id);
          for (auto itb = block_location.begin(); itb != block_location.end(); itb++)
          {
            proxy_proto::locationInfo t_location = block_location[itb->first];
            auto new_cluster = g_main_plan.add_clusters();
            new_cluster->set_cluster_id(t_location.cluster_id());
            new_cluster->set_proxy_ip(t_location.proxy_ip());
            new_cluster->set_proxy_port(t_location.proxy_port());
            for (int ii = 0; ii < int(t_location.blockkeys_size()); ii++)
            {
              new_cluster->add_datanodeip(t_location.datanodeip(ii));
              new_cluster->add_datanodeport(t_location.datanodeport(ii));
              new_cluster->add_blockkeys(t_location.blockkeys(ii));
              new_cluster->add_blockids(t_location.blockids(ii));
            }
          }
          grpc::ClientContext context_m;
          proxy_proto::RecalReply response_m;
          std::string chosen_proxy_m = m_cluster_table[g_cluster_id].proxy_ip + ":" + std::to_string(m_cluster_table[g_cluster_id].proxy_port);
          grpc::Status stat1 = m_proxy_ptrs[chosen_proxy_m]->mainRecal(&context_m, g_main_plan, &response_m);
          if (IF_DEBUG)
          {
            std::cout << "Selected main proxy " << chosen_proxy_m << std::endl;
          }
        };

        // help
        auto send_help_plan = [this, block_location, g_cluster_id, block_size, g_m](int first)
        {
          proxy_proto::helpRecalPlan g_help_plan;
          proxy_proto::locationInfo t_location = block_location.at(first);
          g_help_plan.set_mainproxyip(m_cluster_table[g_cluster_id].proxy_ip);
          // port to accept data: mainproxy_port + cluster_id + 2
          g_help_plan.set_mainproxyport(m_cluster_table[g_cluster_id].proxy_port + 1);
          for (int ii = 0; ii < int(t_location.blockkeys_size()); ii++)
          {
            g_help_plan.add_datanodeip(t_location.datanodeip(ii));
            g_help_plan.add_datanodeport(t_location.datanodeport(ii));
            g_help_plan.add_blockkeys(t_location.blockkeys(ii));
            g_help_plan.add_blockids(t_location.blockids(ii));
          }
          g_help_plan.set_if_partial_decoding(m_encode_parameters.partial_decoding);
          g_help_plan.set_block_size(block_size);
          g_help_plan.set_parity_num(g_m);
          grpc::ClientContext context_h;
          proxy_proto::RecalReply response_h;
          std::string chosen_proxy_h = t_location.proxy_ip() + ":" + std::to_string(t_location.proxy_port());
          grpc::Status stat = m_proxy_ptrs[chosen_proxy_h]->helpRecal(&context_h, g_help_plan, &response_h);
          if (IF_DEBUG)
          {
            std::cout << "Selected helper proxy " << chosen_proxy_h << std::endl;
          }
        };
        try
        {
          if (IF_DEBUG)
          {
            std::cout << "[Global Parities Recalculation] Send main and help proxy plans!" << std::endl;
          }
          std::thread my_main_thread(send_main_plan);
          std::vector<std::thread> senders;
          for (auto itb = block_location.begin(); itb != block_location.end(); itb++)
          {
            if (itb->first != g_cluster_id)
            {
              // send_help_plan(itb->first);
              senders.push_back(std::thread(send_help_plan, itb->first));
            }
          }
          for (int j = 0; j < int(senders.size()); j++)
          {
            senders[j].join();
          }
          my_main_thread.join();
        }
        catch (const std::exception &e)
        {
          std::cerr << e.what() << '\n';
        }
        // check
        proxy_proto::AskIfSuccess ask_c0;
        ask_c0.set_step(0);
        grpc::ClientContext context_c0;
        proxy_proto::RepIfSuccess response_c0;
        std::string chosen_proxy_c0 = m_cluster_table[g_cluster_id].proxy_ip + ":" + std::to_string(m_cluster_table[g_cluster_id].proxy_port);
        grpc::Status stat_c0 = m_proxy_ptrs[chosen_proxy_c0]->checkStep(&context_c0, ask_c0, &response_c0);
        if (stat_c0.ok() && response_c0.ifsuccess() && IF_DEBUG)
        {
          std::cout << "[MERGE] global parity block recalculate success for Stripe" << l_stripe_id << std::endl;
        }
        gettimeofday(&g_end_time, NULL);
        temp_time = g_end_time.tv_sec - g_start_time.tv_sec + (g_end_time.tv_usec - g_start_time.tv_usec) * 1.0 / 1000000;
        t_gc += temp_time;

        // local parity blocks recalculation
        if (IF_DEBUG)
        {
          std::cout << "[MERGE] Start to recalculate local parity blocks for Stripe" << l_stripe_id << std::endl;
        }
        temp_time = 0.0;
        gettimeofday(&l_start_time, NULL);
        auto send_l_main_plan = [this, k, l, block_size, &l_main_plan, &parity_location, &l_cluster_id, l_stripe_id](int gid) mutable
        {
          l_main_plan[gid].set_type(false);
          l_main_plan[gid].set_k(k);
          l_main_plan[gid].set_l(l);
          l_main_plan[gid].set_group_id(gid);
          l_main_plan[gid].set_block_size(block_size);
          l_main_plan[gid].set_if_partial_decoding(m_encode_parameters.partial_decoding);
          l_main_plan[gid].set_stripe_id(l_stripe_id);
          for (auto itb = parity_location[gid].begin(); itb != parity_location[gid].end(); itb++)
          {
            proxy_proto::locationInfo t_location = parity_location[gid][itb->first];
            auto new_cluster = l_main_plan[gid].add_clusters();
            new_cluster->set_cluster_id(t_location.cluster_id());
            new_cluster->set_proxy_ip(t_location.proxy_ip());
            new_cluster->set_proxy_port(t_location.proxy_port());
            for (int ii = 0; ii < int(t_location.blockkeys_size()); ii++)
            {
              new_cluster->add_datanodeip(t_location.datanodeip(ii));
              new_cluster->add_datanodeport(t_location.datanodeport(ii));
              new_cluster->add_blockkeys(t_location.blockkeys(ii));
            }
          }
          grpc::ClientContext context_m;
          proxy_proto::RecalReply response_m;
          std::string chosen_proxy_m = m_cluster_table[l_cluster_id[gid]].proxy_ip + ":" + std::to_string(m_cluster_table[l_cluster_id[gid]].proxy_port);
          grpc::Status stat1 = m_proxy_ptrs[chosen_proxy_m]->mainRecal(&context_m, l_main_plan[gid], &response_m);
          if (IF_DEBUG)
          {
            std::cout << "Selected main proxy " << chosen_proxy_m << std::endl;
          }
        };
        auto send_l_help_plan = [this, &parity_location, &l_cluster_id, block_size](int first, int gid)
        {
          proxy_proto::helpRecalPlan l_help_plan;
          proxy_proto::locationInfo t_location = parity_location[gid].at(first);
          l_help_plan.set_mainproxyip(m_cluster_table[l_cluster_id[gid]].proxy_ip);
          l_help_plan.set_mainproxyport(m_cluster_table[l_cluster_id[gid]].proxy_port + 1);
          for (int ii = 0; ii < int(t_location.blockkeys_size()); ii++)
          {
            l_help_plan.add_datanodeip(t_location.datanodeip(ii));
            l_help_plan.add_datanodeport(t_location.datanodeport(ii));
            l_help_plan.add_blockkeys(t_location.blockkeys(ii));
          }
          l_help_plan.set_if_partial_decoding(m_encode_parameters.partial_decoding);
          l_help_plan.set_block_size(block_size);
          l_help_plan.set_parity_num(1);
          grpc::ClientContext context_h;
          proxy_proto::RecalReply response_h;
          std::string chosen_proxy_h = t_location.proxy_ip() + ":" + std::to_string(t_location.proxy_port());
          grpc::Status stat = m_proxy_ptrs[chosen_proxy_h]->helpRecal(&context_h, l_help_plan, &response_h);
          if (IF_DEBUG)
          {
            std::cout << "Selected helper proxy " << chosen_proxy_h << std::endl;
          }
        };
        for (int i = 0; i < l; i++)
        {
          try
          {
            if (IF_DEBUG)
            {
              std::cout << "[Local Parities Recalculation] Send main and help proxy plans!" << std::endl;
            }
            std::thread my_l_main_thread(send_l_main_plan, i);
            std::vector<std::thread> senders;
            for (auto itb = parity_location[i].begin(); itb != parity_location[i].end(); itb++)
            {
              if (itb->first != l_cluster_id[i])
              {
                senders.push_back(std::thread(send_l_help_plan, itb->first, i));
              }
            }
            for (int j = 0; j < int(senders.size()); j++)
            {
              senders[j].join();
            }
            my_l_main_thread.join();
          }
          catch (const std::exception &e)
          {
            std::cerr << e.what() << '\n';
          }
          // check
          proxy_proto::AskIfSuccess ask_c1;
          ask_c1.set_step(1);
          grpc::ClientContext context_c1;
          proxy_proto::RepIfSuccess response_c1;
          std::string chosen_proxy_c1 = m_cluster_table[l_cluster_id[i]].proxy_ip + ":" + std::to_string(m_cluster_table[l_cluster_id[i]].proxy_port);
          grpc::Status stat_c1 = m_proxy_ptrs[chosen_proxy_c1]->checkStep(&context_c1, ask_c1, &response_c1);
          if (stat_c1.ok() && response_c1.ifsuccess() && IF_DEBUG)
          {
            std::cout << "[MERGE] local parity block recalculate success for Stripe" << l_stripe_id  << ", Group " << i << std::endl;
          }
        }
        gettimeofday(&l_end_time, NULL);
        temp_time = l_end_time.tv_sec - l_start_time.tv_sec + (l_end_time.tv_usec - l_start_time.tv_usec) * 1.0 / 1000000;
        t_lc += temp_time;

        // send delete old parity blocks request
        grpc::ClientContext del_context;
        proxy_proto::DelReply del_reply;
        old_parities.set_stripe_id(-1);
        old_parities.set_key("");
        // randomly select a proxy
        int idx = rand_num(int(old_parities_cluster_set.size()));
        int del_cluster_id = *(std::next(old_parities_cluster_set.begin(), idx));
        std::string del_chosen_proxy = m_cluster_table[del_cluster_id].proxy_ip + ":" + std::to_string(m_cluster_table[del_cluster_id].proxy_port);
        grpc::Status del_status = m_proxy_ptrs[del_chosen_proxy]->deleteBlock(&del_context, old_parities, &del_reply);
        if (del_status.ok() && del_reply.ifcommit())
        {
          std::cout << "[MERGE] Delete old parity blocks success!" << std::endl;
        }


        // data block relocation
        if (int(block_to_move_key.size()) > 0)
        {
          if (IF_DEBUG)
          {
            std::cout << "[MERGE] Start to relocate data blocks for Stripe" << l_stripe_id << std::endl;
          }
          temp_time = 0.0;
          gettimeofday(&d_start_time, NULL);
          proxy_proto::blockRelocPlan b_reloc_plan;
          for (int i = 0; i < int(block_to_move_key.size()); i++)
          {
            int src_node_id = block_src_node[i];
            int des_node_id = block_des_node[i];
            b_reloc_plan.add_blocktomove(block_to_move_key[i]);
            b_reloc_plan.add_fromdatanodeip(m_node_table[src_node_id].node_ip);
            b_reloc_plan.add_fromdatanodeport(m_node_table[src_node_id].node_port);
            b_reloc_plan.add_todatanodeip(m_node_table[des_node_id].node_ip);
            b_reloc_plan.add_todatanodeport(m_node_table[des_node_id].node_port);
          }
          b_reloc_plan.set_block_size(block_size);
          int r_cluster_id = rand_num(m_num_of_Clusters);
          std::string chosen_proxy_b = m_cluster_table[r_cluster_id].proxy_ip + ":" + std::to_string(m_cluster_table[r_cluster_id].proxy_port);
          grpc::ClientContext context_b;
          proxy_proto::blockRelocReply response_b;
          grpc::Status stat_b = m_proxy_ptrs[chosen_proxy_b]->blockReloc(&context_b, b_reloc_plan, &response_b);
          // check
          proxy_proto::AskIfSuccess ask_c2;
          ask_c2.set_step(2);
          grpc::ClientContext context_c2;
          proxy_proto::RepIfSuccess response_c2;
          grpc::Status stat_c2 = m_proxy_ptrs[chosen_proxy_b]->checkStep(&context_c2, ask_c2, &response_c2);
          if (stat_c2.ok() && response_c2.ifsuccess() && IF_DEBUG)
          {
            std::cout << "[MERGE] block relocaltion success!" << std::endl;
          }
          gettimeofday(&d_end_time, NULL);
          temp_time = d_end_time.tv_sec - d_start_time.tv_sec + (d_end_time.tv_usec - d_start_time.tv_usec) * 1.0 / 1000000;
          // for the implementation of the relocation process: src_node -> proxy -> des_node, the time should be halved
          // next optimizable direction: src_node -> des_node
          temp_time /= 2;
          t_lc += temp_time * double(num2mov_k) / double(block_to_move_key.size());
          t_dc += temp_time * double(num2mov_v) / double(block_to_move_key.size());
        }

        // update stripes meta information
        std::unordered_set<int>::iterator iter;
        for (iter = old_stripe_id_set.begin(); iter != old_stripe_id_set.end(); iter++)
        {
          auto its = m_stripe_table.find(*iter);
          if (its != m_stripe_table.end())
          {
            m_stripe_table.erase(its);
          }
        }
        m_stripe_table[l_stripe_id] = larger_stripe;
        m_cur_stripe_id++;
        s_merge_group.push_back(l_stripe_id);
        stripe_cnt += num_of_stripes;
        
        std::cout << "[Merging Stage " << m_merge_degree + 1 << "] Process " << stripe_cnt << "/" << tot_stripe_num 
                  << "  lc:" << t_lc << " gc:" << t_gc << " dc:" << t_dc << std::endl;
      }
      new_merge_groups.push_back(s_merge_group);
    }
    // update m_merge_groups
    m_merge_groups.clear();
    m_merge_groups = new_merge_groups;
    mergeReplyClient->set_ifmerged(true);
    mergeReplyClient->set_lc(t_lc);
    mergeReplyClient->set_gc(t_gc);
    mergeReplyClient->set_dc(t_dc);

    if (IF_DEBUG)
    {
      // print the result
      std::cout << std::endl;
      std::cout << "After Merge of this stage:" << std::endl;
      for (int i = 0; i < m_num_of_Clusters; i++)
      {
        Cluster &t_cluster = m_cluster_table[i];
        if (int(t_cluster.blocks.size()) > 0)
        {
          std::cout << "Cluster " << i << ": ";
          for (auto it = t_cluster.blocks.begin(); it != t_cluster.blocks.end(); it++)
          {
            std::cout << "[" << (*it)->block_key << ":S" << (*it)->map2stripe << "G" << (*it)->map2group << "N" << (*it)->map2node << "] ";
          }
          std::cout << std::endl;
        }
      }
      std::cout << "Merge Group: ";
      for (auto it1 = m_merge_groups.begin(); it1 != m_merge_groups.end(); it1++)
      {
        std::cout << "[ ";
        for (auto it2 = (*it1).begin(); it2 != (*it1).end(); it2++)
        {
          std::cout << (*it2) << " ";
        }
        std::cout << "] ";
      }
      std::cout << std::endl;
      std::cout << std::endl;
    }
    m_merge_degree += 1;

    return grpc::Status::OK;
  }

  void CoordinatorImpl::blocks_in_cluster(std::map<char, std::vector<ECProject::Block *>> &block_info, int cluster_id, int stripe_id)
  {
    std::vector<ECProject::Block *> tt, td, tl, tg;
    Cluster &cluster = m_cluster_table[cluster_id];
    std::vector<Block *>::iterator it;
    for (it = cluster.blocks.begin(); it != cluster.blocks.end(); it++)
    {
      Block *block = *it;
      if (block->map2stripe == stripe_id)
      {
        tt.push_back(block);
        if (block->block_type == 'D')
        {
          td.push_back(block);
        }
        else if (block->block_type == 'L')
        {
          tl.push_back(block);
        }
        else
        {
          tg.push_back(block);
        }
      }
    }
    block_info['T'] = tt;
    block_info['D'] = td;
    block_info['L'] = tl;
    block_info['G'] = tg;
  }

  void CoordinatorImpl::find_max_group(int &max_group_id, int &max_group_num, int cluster_id, int stripe_id)
  {
    int group_cnt[5] = {0};
    Cluster &cluster = m_cluster_table[cluster_id];
    std::vector<Block *>::iterator it;
    for (it = cluster.blocks.begin(); it != cluster.blocks.end(); it++)
    {
      if ((*it)->map2stripe == stripe_id)
      {
        group_cnt[(*it)->map2group]++;
      }
    }
    for (int i = 0; i <= m_encode_parameters.l_localparityblock; i++)
    {
      if (group_cnt[i] > max_group_num)
      {
        max_group_id = i;
        max_group_num = group_cnt[i];
      }
    }
  }

  int CoordinatorImpl::count_block_num(char type, int cluster_id, int stripe_id, int group_id)
  {
    int cnt = 0;
    Cluster &cluster = m_cluster_table[cluster_id];
    std::vector<Block *>::iterator it;
    for (it = cluster.blocks.begin(); it != cluster.blocks.end(); it++)
    {
      Block *block = *it;
      if (block->map2stripe == stripe_id)
      {
        if (group_id == -1)
        {
          if (type == 'T')
          {
            cnt++;
          }
          else if (block->block_type == type)
          {
            cnt++;
          }
        }
        else if (int(block->map2group) == group_id)
        {
          if (type == 'T')
          {
            cnt++;
          }
          else if (block->block_type == type)
          {
            cnt++;
          }
        }
      }
    }
    if (cnt == 0)
    {
      cluster.stripes.erase(stripe_id);
    }
    return cnt;
  }

  // find out if any specific type of block from the stripe exists in the cluster
  bool CoordinatorImpl::find_block(char type, int cluster_id, int stripe_id)
  {
    Cluster &cluster = m_cluster_table[cluster_id];
    std::vector<Block *>::iterator it;
    for (it = cluster.blocks.begin(); it != cluster.blocks.end(); it++)
    {
      if (stripe_id != -1 && int((*it)->map2stripe) == stripe_id && (*it)->block_type == type)
      {
        return true;
      }
      else if (stripe_id == -1 && (*it)->block_type == type)
      {
        return true;
      }
    }
    return false;
  }
} // namespace ECProject
